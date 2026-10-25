#include "linx_device.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "bsp_audio.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_opus_dec.h"
#include "esp_random.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "esp_transport_ssl.h"
#include "esp_transport_ws.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "linx_protocol.h"
#include "linx_wifi_portal.h"
#include "nvs.h"
#include "nvs_flash.h"

#define CONNECT_TIMEOUT_MS 5000
#define READY_TIMEOUT_MS 10000
#define TIME_SYNC_TIMEOUT_MS 60000
#define SCAN_TIMEOUT_MS 10000
#define SCAN_RECORD_CAPACITY 16U
#define SETUP_TIMEOUT_MS 300000
#define SETUP_SUCCESS_MS 3000
#define RESPONSE_TIMEOUT_MS 30000
#define IDLE_TIMEOUT_MS 30000
#define SEND_TIMEOUT_MS 1000
#define READ_TIMEOUT_MS 20
#define FRAME_TIMEOUT_MS 1000
#define WORKER_STACK_BYTES (16 * 1024)
#define OTA_URL "https://xrobo.qiniuapi.com/v1/ota/"
#define CLIENT_AGENT "LinxBox/0.2 (FoloToy AI Passport)"

/* 两个队列都是快照，录音和回复不会积攒成与说话时长成正比的缓存。 */
static QueueHandle_t commands;
static QueueHandle_t events;
static atomic_uint active_generation;
static atomic_bool active;
static atomic_bool listening;
/* Gateway 的网络租约不经过语音单槽，不改变录音代号或采集意图。 */
static atomic_bool network_wanted;
static atomic_bool network_ready;
static atomic_bool network_provisioning;
static atomic_uint network_epoch;
static atomic_uint network_close_ack;
static atomic_int network_error;
static char network_mac[18];
static int64_t network_retry_at;

static atomic_bool finish_pending;
/* 软取消与硬释放都不能被随后的 PREPARE 快照覆盖。 */
static atomic_bool discard_transport;
static atomic_bool cancel_turn_pending;
static portMUX_TYPE command_state_lock = portMUX_INITIALIZER_UNLOCKED;
typedef struct {
    linx_voice_command_t command;
    uint32_t generation;
    linx_reply_context_t context;
    char gateway_request_id[513];
    char gateway_agent_session_id[513];
} voice_target_t;
/* 只保留最新目标，不含密码；预热中的同目标请求可直接接管原握手。 */
static voice_target_t requested_target;
static portMUX_TYPE wifi_state_lock = portMUX_INITIALIZER_UNLOCKED;
ESP_EVENT_DEFINE_BASE(LINX_WIFI_SYNC_EVENT);

typedef struct {
    uint32_t generation;
    esp_netif_t *netif;
    esp_netif_t *ap_netif;
    linx_wifi_portal_t *portal;
    int64_t setup_deadline;
    bool trial;
    atomic_bool trial_associated;
    char trial_ssid[LINX_WIFI_SSID_CAPACITY];
    esp_event_handler_instance_t wifi_handler;
    esp_event_handler_instance_t ip_handler;
    esp_event_handler_instance_t sync_handler;
    atomic_uint disconnect_count;
    atomic_bool connect_pending;
    atomic_bool event_drained;
    uint32_t drain_number;
    bool trial_accepting;
    bool own_loop;
    bool wifi_initialized;
    bool wifi_started;
    bool scan_started;
    atomic_bool scan_done;
    atomic_bool scan_failed;
    bool own_sntp;
    bool audio_awake;
    bool ws_log_muted;
    esp_log_level_t ws_log_level;
    atomic_bool connected;
    atomic_bool disconnected;
    esp_transport_handle_t ssl;
    esp_transport_handle_t ws;
    void *decoder;
    linx_gateway_t gateway;
    linx_receiver_t receiver;
    linx_audio_t audio;
    char session_id[LINX_SESSION_CAPACITY];
    char device_id[18];
    char client_id[37];
    char scratch[LINX_MESSAGE_CAPACITY + 1U];
    uint8_t pcm[LINX_PCM_OUTPUT_BYTES];
    uint8_t microphone[LINX_PCM_FRAME_BYTES];
    uint8_t control[125];
    size_t control_used;
    size_t http_used;
    bool http_overflow;
    bool hello;
    bool speaking;
    bool waiting;
    bool submitted;
    bool completed;
    bool turn_active;
    bool voice;
    bool network_only;
    char gateway_request_id[513];
    char gateway_agent_session_id[513];
    bool preparing;
    bool draining;
    bool drain_stopped;
    linx_voice_command_t requested_command;
    uint32_t handled_generation;
    linx_reply_context_t context;
    int64_t idle_deadline;
    unsigned frame_opcode;
    bool frame_fin;
    size_t frame_size;
    int64_t last_activity;
} voice_session_t;

/* SDK 停服失败时只保留这一个待收尾对象，不会另启服务。 */
static voice_session_t *pending_cleanup;
/* Wi-Fi 事件回调始终指向这个稳定对象，语音传输可独立释放。 */
static voice_session_t *retained;

static void clear_sensitive(void *data, size_t size)
{
    volatile uint8_t *bytes = data;
    while (size--) *bytes++ = 0;
}

static bool current(uint32_t generation)
{
    return atomic_load(&active) && atomic_load(&active_generation) == generation;
}

static bool same_context(const linx_reply_context_t *left, const linx_reply_context_t *right)
{
    return left->scene_id == right->scene_id && left->task_id == right->task_id &&
           left->source_turn_id == right->source_turn_id && left->image_id == right->image_id;
}

static bool same_voice_target(const linx_reply_context_t *left, const char *left_session,
                              const linx_reply_context_t *right, const char *right_session)
{
    if (left_session[0] || right_session[0]) return strcmp(left_session, right_session) == 0;
    return same_context(left, right);
}

/* 连接生命期独立于页面代号；代号只限制事件发布及本条采集/播放。 */
static bool session_current(voice_session_t *session)
{
    if (session->network_only) return atomic_load(&network_wanted) && !atomic_load(&network_provisioning);
    if (!session->voice) return current(session->generation);
    if (atomic_load(&discard_transport)) return false;
    if (session->preparing) {
        taskENTER_CRITICAL(&command_state_lock);
        voice_target_t target = requested_target;
        taskEXIT_CRITICAL(&command_state_lock);
        if (target.command != LINX_VOICE_PREPARE && target.command != LINX_VOICE_START) return false;
        if (target.generation == atomic_load(&active_generation) &&
            same_voice_target(&session->context, session->gateway_agent_session_id,
                              &target.context, target.gateway_agent_session_id)) {
            session->generation = target.generation;
            session->requested_command = target.command;
            memcpy(session->gateway_request_id, target.gateway_request_id, sizeof(session->gateway_request_id));
        }
    }
    return true;
}

static void publish(uint32_t generation, linx_voice_state_t state, linx_voice_error_t error,
                    const char *activation_code)
{
    if (!current(generation)) return;
    linx_voice_event_t event = {.generation = generation, .state = state, .error = error};
    if (activation_code) memcpy(event.activation_code, activation_code, sizeof(event.activation_code));
    xQueueOverwrite(events, &event);
}

static linx_voice_error_t sdk_error(esp_err_t error, linx_voice_error_t fallback)
{
    return error == ESP_ERR_NO_MEM ? LINX_VOICE_NO_MEMORY : fallback;
}

/* 在唯一 Wi-Fi owner 内调用；先公布失效再等业务 WSS 确认关闭。 */
static void network_invalidate(void)
{
    if (atomic_exchange(&network_ready, false)) atomic_fetch_add(&network_epoch, 1U);
}

static bool network_close_wait(void)
{
    network_invalidate();
    uint32_t epoch = atomic_load(&network_epoch);
    int64_t deadline = esp_timer_get_time() + CONNECT_TIMEOUT_MS * 1000LL;
    while (atomic_load(&network_wanted) && atomic_load(&network_close_ack) != epoch) {
        if (esp_timer_get_time() >= deadline) {
            atomic_store(&network_error, LINX_VOICE_TIMEOUT);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(READ_TIMEOUT_MS));
    }
    return true;
}

static void wifi_event(void *user, esp_event_base_t base, int32_t id, void *data)
{
    voice_session_t *session = user;
    taskENTER_CRITICAL(&wifi_state_lock);
    if (base == LINX_WIFI_SYNC_EVENT && data && (uint32_t)id == session->drain_number &&
        *(const uint32_t *)data == session->generation) atomic_store(&session->event_drained, true);
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED && session->trial && session->trial_accepting) {
        const wifi_event_sta_connected_t *connected = data;
        if (connected && connected->ssid_len == strlen(session->trial_ssid) &&
            !memcmp(connected->ssid, session->trial_ssid, connected->ssid_len))
            atomic_store(&session->trial_associated, true);
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP &&
        (!session->trial || (session->trial_accepting && atomic_load(&session->trial_associated)))) {
        atomic_store(&session->connected, true);
        atomic_store(&session->connect_pending, false);
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        const wifi_event_sta_scan_done_t *done = data;
        atomic_store(&session->scan_failed, !done || done->status != 0);
        atomic_store(&session->scan_done, true);
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        atomic_fetch_add(&session->disconnect_count, 1U);
        if (!session->trial || !session->trial_accepting) atomic_store(&session->connect_pending, false);
        const wifi_event_sta_disconnected_t *disconnected = data;
        if (!session->trial || (session->trial_accepting && (!disconnected ||
            (disconnected->ssid_len == strlen(session->trial_ssid) &&
             !memcmp(disconnected->ssid, session->trial_ssid, disconnected->ssid_len))))) {
            atomic_store(&session->connected, false);
            atomic_store(&session->connect_pending, false);
            atomic_store(&session->trial_associated, false);
            atomic_store(&session->disconnected, true);
            network_invalidate();
        }
    }
    taskEXIT_CRITICAL(&wifi_state_lock);
}

static bool client_id_valid(const char *client_id)
{
    if (strlen(client_id) != 36U || client_id[14] != '4' || !strchr("89ab", client_id[19])) return false;
    for (size_t i = 0; i < 36U; ++i) {
        bool dash = i == 8U || i == 13U || i == 18U || i == 23U;
        if (dash ? client_id[i] != '-' : strchr("0123456789abcdef", client_id[i]) == NULL) return false;
    }
    return true;
}

static linx_voice_error_t identity(voice_session_t *session)
{
    uint8_t mac[6];
    esp_err_t result = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (result != ESP_OK) return sdk_error(result, LINX_VOICE_WIFI_FAILED);
    snprintf(session->device_id, sizeof(session->device_id), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    nvs_handle_t store;
    result = nvs_open("linx", NVS_READWRITE, &store);
    if (result != ESP_OK) return sdk_error(result, LINX_VOICE_WIFI_FAILED);
    size_t size = sizeof(session->client_id);
    result = nvs_get_str(store, "client_id", session->client_id, &size);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        uint8_t uuid[16];
        esp_fill_random(uuid, sizeof(uuid));
        uuid[6] = (uuid[6] & 0x0fU) | 0x40U;
        uuid[8] = (uuid[8] & 0x3fU) | 0x80U;
        snprintf(session->client_id, sizeof(session->client_id),
                 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                 uuid[0], uuid[1], uuid[2], uuid[3], uuid[4], uuid[5], uuid[6], uuid[7],
                 uuid[8], uuid[9], uuid[10], uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]);
        result = nvs_set_str(store, "client_id", session->client_id);
        if (result == ESP_OK) result = nvs_commit(store);
    } else if (result == ESP_OK && (size != sizeof(session->client_id) ||
                                  !client_id_valid(session->client_id))) {
        result = ESP_ERR_INVALID_STATE;
    }
    nvs_close(store);
    return result == ESP_OK ? LINX_VOICE_OK : sdk_error(result, LINX_VOICE_WIFI_FAILED);
}

static linx_voice_error_t wifi_prepare(voice_session_t *session, bool require_identity)
{
    esp_err_t result = nvs_flash_init();
    if (result != ESP_OK) return sdk_error(result, LINX_VOICE_WIFI_FAILED);
    if (require_identity) {
        linx_voice_error_t error = identity(session);
        if (error != LINX_VOICE_OK) return error;
    }
    result = esp_netif_init();
    if (result != ESP_OK) return sdk_error(result, LINX_VOICE_WIFI_FAILED);
    result = esp_event_loop_create_default();
    session->own_loop = result == ESP_OK;
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) return sdk_error(result, LINX_VOICE_WIFI_FAILED);
    /* 默认创建函数会 assert；逐步创建才能在分配失败后返回页面并允许重试。 */
    if (esp_netif_get_handle_from_ifkey("WIFI_STA_DEF")) return LINX_VOICE_WIFI_FAILED;
    esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_WIFI_STA();
    session->netif = esp_netif_new(&netif_config);
    if (!session->netif) return LINX_VOICE_NO_MEMORY;
    result = esp_netif_attach_wifi_station(session->netif);
    if (result == ESP_OK) result = esp_wifi_set_default_wifi_sta_handlers();
    if (result != ESP_OK) return sdk_error(result, LINX_VOICE_WIFI_FAILED);
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    result = esp_wifi_init(&init);
    if (result != ESP_OK) return sdk_error(result, LINX_VOICE_WIFI_FAILED);
    session->wifi_initialized = true;
    result = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event,
                                                session, &session->wifi_handler);
    if (result == ESP_OK) result = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                                      wifi_event, session, &session->ip_handler);
    if (result != ESP_OK) return sdk_error(result, LINX_VOICE_WIFI_FAILED);
    result = esp_wifi_set_storage(WIFI_STORAGE_FLASH);
    if (result == ESP_OK) result = esp_wifi_set_mode(WIFI_MODE_STA);
    return result == ESP_OK ? LINX_VOICE_OK : sdk_error(result, LINX_VOICE_WIFI_FAILED);
}

static linx_voice_error_t wifi_radio_start(voice_session_t *session)
{
    if (!session_current(session)) return LINX_VOICE_OK;
    esp_err_t result = esp_wifi_start();
    if (result != ESP_OK) return sdk_error(result, LINX_VOICE_WIFI_FAILED);
    session->wifi_started = true;
    return LINX_VOICE_OK;
}

static linx_voice_error_t wifi_connect_wait(voice_session_t *session)
{
    if (!session_current(session)) return LINX_VOICE_OK;
    atomic_store(&session->connect_pending, true);
    esp_err_t result = esp_wifi_connect();
    if (result != ESP_OK) {
        atomic_store(&session->connect_pending, false);
        return sdk_error(result, LINX_VOICE_WIFI_FAILED);
    }
    int64_t deadline = esp_timer_get_time() + READY_TIMEOUT_MS * 1000LL;
    if (session->setup_deadline && deadline > session->setup_deadline) deadline = session->setup_deadline;
    while (session_current(session) && !atomic_load(&session->connected)) {
        if (atomic_load(&session->disconnected)) return LINX_VOICE_WIFI_FAILED;
        if (esp_timer_get_time() >= deadline) return LINX_VOICE_TIMEOUT;
        vTaskDelay(pdMS_TO_TICKS(READ_TIMEOUT_MS));
    }
    return LINX_VOICE_OK;
}

/* 设备上保存的网络优先；编译配置仅为尚未配网的开发设备提供回退。 */
static linx_voice_error_t wifi_start(voice_session_t *session)
{
    linx_voice_error_t error = wifi_prepare(session, true);
    if (error != LINX_VOICE_OK) return error;
    wifi_config_t config = {0};
    esp_err_t result = esp_wifi_get_config(WIFI_IF_STA, &config);
    if (result != ESP_OK) {
        clear_sensitive(&config, sizeof(config));
        return sdk_error(result, LINX_VOICE_WIFI_FAILED);
    }
    if (!config.sta.ssid[0] && CONFIG_LINX_WIFI_SSID[0]) {
        size_t ssid_size = strlen(CONFIG_LINX_WIFI_SSID);
        size_t password_size = strlen(CONFIG_LINX_WIFI_PASSWORD);
        if (ssid_size > sizeof(config.sta.ssid) || password_size > sizeof(config.sta.password)) {
            clear_sensitive(&config, sizeof(config));
            return LINX_VOICE_NOT_CONFIGURED;
        }
        memcpy(config.sta.ssid, CONFIG_LINX_WIFI_SSID, ssid_size);
        memcpy(config.sta.password, CONFIG_LINX_WIFI_PASSWORD, password_size);
        result = esp_wifi_set_config(WIFI_IF_STA, &config);
        clear_sensitive(&config, sizeof(config));
        if (result != ESP_OK) return sdk_error(result, LINX_VOICE_WIFI_FAILED);
    } else {
        bool configured = config.sta.ssid[0] != 0;
        clear_sensitive(&config, sizeof(config));
        if (!configured) return LINX_VOICE_NOT_CONFIGURED;
    }
    error = wifi_radio_start(session);
    return error == LINX_VOICE_OK ? wifi_connect_wait(session) : error;
}

static linx_wifi_security_t wifi_security(wifi_auth_mode_t authmode)
{
    if (authmode == WIFI_AUTH_OPEN) return LINX_WIFI_OPEN;
    /* 保持 IDF 的 WPA2 最低认证要求；WPA-only/TKIP 不提供密码输入。 */
    if (authmode == WIFI_AUTH_WPA2_PSK ||
        authmode == WIFI_AUTH_WPA_WPA2_PSK || authmode == WIFI_AUTH_WPA3_PSK ||
        authmode == WIFI_AUTH_WPA2_WPA3_PSK) return LINX_WIFI_PERSONAL;
    return LINX_WIFI_UNSUPPORTED;
}

static linx_voice_error_t wifi_scan_collect(voice_session_t *session, linx_voice_event_t *out)
{
    wifi_scan_config_t config = {.show_hidden = false};
    esp_err_t scanned = esp_wifi_scan_start(&config, false);
    if (scanned != ESP_OK) return sdk_error(scanned, LINX_VOICE_WIFI_FAILED);
    session->scan_started = true;
    int64_t deadline = esp_timer_get_time() + SCAN_TIMEOUT_MS * 1000LL;
    while (session_current(session) && !atomic_load(&session->scan_done)) {
        if (esp_timer_get_time() >= deadline) return LINX_VOICE_TIMEOUT;
        vTaskDelay(pdMS_TO_TICKS(READ_TIMEOUT_MS));
    }
    if (!session_current(session)) return LINX_VOICE_OK;
    if (atomic_load(&session->scan_failed)) return LINX_VOICE_WIFI_FAILED;
    wifi_ap_record_t records[SCAN_RECORD_CAPACITY] = {0};
    uint16_t count = SCAN_RECORD_CAPACITY;
    scanned = esp_wifi_scan_get_ap_records(&count, records);
    if (scanned != ESP_OK) return sdk_error(scanned, LINX_VOICE_WIFI_FAILED);
    if (count > SCAN_RECORD_CAPACITY) return LINX_VOICE_WIFI_FAILED;
    /* SDK 返回最强的一组；排序也让同名网络保留信号最强的一条。 */
    for (size_t i = 1; i < count; ++i) {
        wifi_ap_record_t record = records[i];
        size_t before = i;
        while (before > 0 && records[before - 1].rssi < record.rssi) {
            records[before] = records[before - 1];
            --before;
        }
        records[before] = record;
    }
    linx_voice_event_t event = {.generation = session->generation, .state = LINX_VOICE_WIFI_NETWORKS};
    for (size_t i = 0; i < count; ++i) {
        if (!records[i].ssid[0]) continue;
        char ssid[LINX_WIFI_SSID_CAPACITY] = {0};
        memcpy(ssid, records[i].ssid, sizeof(ssid) - 1U);
        linx_wifi_security_t security = wifi_security(records[i].authmode);
        size_t duplicate = 0;
        while (duplicate < event.wifi_count && strcmp(event.wifi_networks[duplicate].ssid, ssid)) ++duplicate;
        if (duplicate < event.wifi_count) {
            if (event.wifi_networks[duplicate].security != security)
                event.wifi_networks[duplicate].security = LINX_WIFI_UNSUPPORTED;
            continue;
        }
        if (event.wifi_count == LINX_WIFI_NETWORK_CAPACITY) continue;
        linx_wifi_network_t *network = &event.wifi_networks[event.wifi_count++];
        memcpy(network->ssid, ssid, sizeof(network->ssid));
        network->rssi = records[i].rssi;
        network->security = security;
    }
    *out = event;
    return LINX_VOICE_OK;
}

static linx_voice_error_t wifi_scan(voice_session_t *session)
{
    linx_voice_error_t result = wifi_prepare(session, false);
    if (result == LINX_VOICE_OK) result = wifi_radio_start(session);
    if (result != LINX_VOICE_OK || !session_current(session)) return result;
    linx_voice_event_t event;
    result = wifi_scan_collect(session, &event);
    if (result == LINX_VOICE_OK && session_current(session)) xQueueOverwrite(events, &event);
    return result;
}

/* 驱动断开完成后，再让同一事件循环经过 barrier，旧 DHCP/IP 事件不能跨入下一轮。 */
static linx_voice_error_t wifi_quiesce(voice_session_t *session)
{
    if (!session->sync_handler) {
        esp_err_t registered = esp_event_handler_instance_register(LINX_WIFI_SYNC_EVENT, ESP_EVENT_ANY_ID,
                                                                   wifi_event, session, &session->sync_handler);
        if (registered != ESP_OK) return sdk_error(registered, LINX_VOICE_WIFI_FAILED);
    }
    taskENTER_CRITICAL(&wifi_state_lock);
    bool await_disconnect = atomic_load(&session->connect_pending) || atomic_load(&session->connected) ||
                            atomic_load(&session->trial_associated);
    session->trial = true;
    session->trial_accepting = false;
    atomic_store(&session->trial_associated, false);
    atomic_store(&session->connected, false);
    unsigned before = atomic_load(&session->disconnect_count);
    taskEXIT_CRITICAL(&wifi_state_lock);
    esp_err_t disconnected = esp_wifi_disconnect();
    if (disconnected != ESP_OK && disconnected != ESP_ERR_WIFI_NOT_CONNECT)
        return sdk_error(disconnected, LINX_VOICE_WIFI_FAILED);
    int64_t deadline = esp_timer_get_time() + CONNECT_TIMEOUT_MS * 1000LL;
    if (session->setup_deadline && deadline > session->setup_deadline) deadline = session->setup_deadline;
    while (disconnected == ESP_OK && await_disconnect && atomic_load(&session->disconnect_count) == before && session_current(session)) {
        if (esp_timer_get_time() >= deadline) return LINX_VOICE_TIMEOUT;
        vTaskDelay(pdMS_TO_TICKS(READ_TIMEOUT_MS));
    }
    if (!session_current(session)) return LINX_VOICE_OK;
    taskENTER_CRITICAL(&wifi_state_lock);
    ++session->drain_number;
    atomic_store(&session->event_drained, false);
    taskEXIT_CRITICAL(&wifi_state_lock);
    esp_err_t posted = esp_event_post(LINX_WIFI_SYNC_EVENT, (int32_t)session->drain_number,
                                     &session->generation, sizeof(session->generation), 0);
    if (posted != ESP_OK) return sdk_error(posted, LINX_VOICE_WIFI_FAILED);
    while (!atomic_load(&session->event_drained) && session_current(session)) {
        if (esp_timer_get_time() >= deadline) return LINX_VOICE_TIMEOUT;
        vTaskDelay(pdMS_TO_TICKS(READ_TIMEOUT_MS));
    }
    return LINX_VOICE_OK;
}

static linx_voice_error_t wifi_trial(voice_session_t *session, const linx_voice_request_t *request,
                                    wifi_config_t *previous)
{
    if (!memchr(request->wifi_network.ssid, 0, sizeof(request->wifi_network.ssid)) ||
        !memchr(request->wifi_password, 0, sizeof(request->wifi_password)) ||
        !request->wifi_network.ssid[0] || request->wifi_network.security == LINX_WIFI_UNSUPPORTED ||
        (unsigned)request->wifi_network.security > LINX_WIFI_UNSUPPORTED ||
        !linx_wifi_password_valid(request->wifi_password, request->wifi_network.security != LINX_WIFI_OPEN))
        return LINX_VOICE_NOT_CONFIGURED;
    wifi_config_t candidate = {0};
    linx_voice_error_t result = LINX_VOICE_OK;
    esp_err_t configured = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (configured != ESP_OK) return sdk_error(configured, LINX_VOICE_WIFI_FAILED);
    if (session->wifi_started) {
        result = wifi_quiesce(session);
        if (result != LINX_VOICE_OK || !session_current(session)) return result;
    }
    /* 每次重试都重新等待本次关联和 IP；不接受上一次网络的残留事件。 */
    taskENTER_CRITICAL(&wifi_state_lock);
    session->trial = true;
    session->trial_accepting = true;
    memcpy(session->trial_ssid, request->wifi_network.ssid, sizeof(session->trial_ssid));
    atomic_store(&session->connected, false);
    atomic_store(&session->disconnected, false);
    atomic_store(&session->trial_associated, false);
    taskEXIT_CRITICAL(&wifi_state_lock);
    memcpy(candidate.sta.ssid, request->wifi_network.ssid, strlen(request->wifi_network.ssid));
    memcpy(candidate.sta.password, request->wifi_password, strlen(request->wifi_password));
    configured = esp_wifi_set_config(WIFI_IF_STA, &candidate);
    if (configured == ESP_OK && !session->wifi_started) result = wifi_radio_start(session);
    else if (configured != ESP_OK) result = sdk_error(configured, LINX_VOICE_WIFI_FAILED);
    if (result == LINX_VOICE_OK) result = wifi_connect_wait(session);
    if (result == LINX_VOICE_OK && session_current(session)) {
        result = wifi_quiesce(session);
        if (result == LINX_VOICE_OK && session_current(session)) {
            /* SDK 同值 set_config 会跳过 NVS：先在 RAM 恢复原值，制造真实配置变化。 */
            configured = esp_wifi_set_config(WIFI_IF_STA, previous);
            if (configured != ESP_OK) result = sdk_error(configured, LINX_VOICE_WIFI_FAILED);
        }
    }
    if (result == LINX_VOICE_OK && session_current(session)) {
        configured = esp_wifi_set_storage(WIFI_STORAGE_FLASH);
        if (configured == ESP_OK && session_current(session))
            configured = esp_wifi_set_config(WIFI_IF_STA, &candidate);
        if (configured != ESP_OK || !session_current(session)) {
            esp_err_t restored = esp_wifi_set_storage(WIFI_STORAGE_FLASH);
            if (restored == ESP_OK) restored = esp_wifi_set_config(WIFI_IF_STA, previous);
            result = sdk_error(restored != ESP_OK ? restored : configured, LINX_VOICE_WIFI_FAILED);
        }
    }
    clear_sensitive(&candidate, sizeof(candidate));
    return result;
}

static linx_voice_error_t wifi_save(voice_session_t *session, const linx_voice_request_t *request)
{
    if (!memchr(request->wifi_network.ssid, 0, sizeof(request->wifi_network.ssid)) ||
        !memchr(request->wifi_password, 0, sizeof(request->wifi_password)) ||
        !request->wifi_network.ssid[0] || request->wifi_network.security == LINX_WIFI_UNSUPPORTED ||
        (unsigned)request->wifi_network.security > LINX_WIFI_UNSUPPORTED ||
        !linx_wifi_password_valid(request->wifi_password, request->wifi_network.security != LINX_WIFI_OPEN))
        return LINX_VOICE_NOT_CONFIGURED;
    linx_voice_error_t result = wifi_prepare(session, false);
    if (result != LINX_VOICE_OK) return result;
    wifi_config_t previous = {0};
    esp_err_t configured = esp_wifi_get_config(WIFI_IF_STA, &previous);
    if (configured != ESP_OK) result = sdk_error(configured, LINX_VOICE_WIFI_FAILED);
    else result = wifi_trial(session, request, &previous);
    if (result == LINX_VOICE_OK && session_current(session))
        publish(session->generation, LINX_VOICE_WIFI_SAVED, LINX_VOICE_OK, NULL);
    clear_sensitive(&previous, sizeof(previous));
    return result;
}

static linx_voice_error_t wifi_setup(voice_session_t *session)
{
    session->setup_deadline = esp_timer_get_time() + SETUP_TIMEOUT_MS * 1000LL;
    linx_voice_error_t result = wifi_prepare(session, false);
    wifi_config_t previous = {0};
    esp_err_t configured = ESP_OK;
    if (result == LINX_VOICE_OK) {
        configured = esp_wifi_get_config(WIFI_IF_STA, &previous);
        if (configured == ESP_OK) configured = esp_wifi_set_storage(WIFI_STORAGE_RAM);
        if (configured != ESP_OK) result = sdk_error(configured, LINX_VOICE_WIFI_FAILED);
    }
    if (result == LINX_VOICE_OK) result = wifi_radio_start(session);
    linx_voice_event_t event = {.generation = session->generation};
    if (result == LINX_VOICE_OK && session_current(session)) result = wifi_scan_collect(session, &event);
    if (result != LINX_VOICE_OK || !session_current(session)) goto done;
    if (esp_netif_get_handle_from_ifkey("WIFI_AP_DEF")) { result = LINX_VOICE_WIFI_FAILED; goto done; }
    esp_netif_config_t ap_config = ESP_NETIF_DEFAULT_WIFI_AP();
    session->ap_netif = esp_netif_new(&ap_config);
    if (!session->ap_netif) { result = LINX_VOICE_NO_MEMORY; goto done; }
    configured = esp_netif_attach_wifi_ap(session->ap_netif);
    if (configured == ESP_OK) configured = esp_wifi_set_default_wifi_ap_handlers();
    if (configured != ESP_OK) { result = sdk_error(configured, LINX_VOICE_WIFI_FAILED); goto done; }
    uint8_t mac[6];
    configured = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (configured != ESP_OK) { result = sdk_error(configured, LINX_VOICE_WIFI_FAILED); goto done; }
    snprintf(event.wifi_ap_ssid, sizeof(event.wifi_ap_ssid), "LinxBox-%02X%02X%02X", mac[3], mac[4], mac[5]);
    uint8_t random[8];
    esp_fill_random(random, sizeof(random));
    for (size_t i = 0; i < sizeof(random); ++i) event.wifi_ap_password[i] = (char)('0' + random[i] % 10U);
    wifi_config_t ap = {0};
    memcpy(ap.ap.ssid, event.wifi_ap_ssid, strlen(event.wifi_ap_ssid));
    ap.ap.ssid_len = (uint8_t)strlen(event.wifi_ap_ssid);
    memcpy(ap.ap.password, event.wifi_ap_password, 8);
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.max_connection = 2;
    /* 先停初始扫描的 STA，再在停机状态配置 AP，避免短暂广播默认开放热点。 */
    configured = esp_wifi_stop();
    if (configured == ESP_OK) session->wifi_started = false;
    if (configured == ESP_OK) configured = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (configured == ESP_OK) configured = esp_wifi_set_config(WIFI_IF_AP, &ap);
    clear_sensitive(&ap, sizeof(ap));
    clear_sensitive(random, sizeof(random));
    if (configured != ESP_OK) { result = sdk_error(configured, LINX_VOICE_WIFI_FAILED); goto done; }
    result = wifi_radio_start(session);
    if (result != LINX_VOICE_OK || !session_current(session)) goto done;
    configured = linx_wifi_portal_start(&session->portal, event.wifi_networks, event.wifi_count);
    if (configured != ESP_OK) { result = sdk_error(configured, LINX_VOICE_WIFI_FAILED); goto done; }
    event.state = LINX_VOICE_WIFI_PORTAL;
    if (session_current(session)) xQueueOverwrite(events, &event);
    while (session_current(session)) {
        if (esp_timer_get_time() >= session->setup_deadline) { result = LINX_VOICE_TIMEOUT; break; }
        linx_voice_request_t candidate = {.generation = session->generation};
        if (!linx_wifi_portal_poll(session->portal, &candidate.wifi_network, candidate.wifi_password)) {
            vTaskDelay(pdMS_TO_TICKS(READ_TIMEOUT_MS));
            continue;
        }
        publish(session->generation, LINX_VOICE_WIFI_CONNECTING, LINX_VOICE_OK, NULL);
        linx_wifi_portal_status(session->portal, LINX_WIFI_PORTAL_CONNECTING);
        result = wifi_trial(session, &candidate, &previous);
        clear_sensitive(&candidate, sizeof(candidate));
        if (result != LINX_VOICE_OK) {
            linx_wifi_portal_status(session->portal, LINX_WIFI_PORTAL_ERROR);
            event.error = result;
            if (session_current(session)) xQueueOverwrite(events, &event);
            continue;
        }
        if (!session_current(session)) break;
        linx_wifi_portal_status(session->portal, LINX_WIFI_PORTAL_SAVED);
        int64_t response_deadline = esp_timer_get_time() + SETUP_SUCCESS_MS * 1000LL;
        if (response_deadline > session->setup_deadline) response_deadline = session->setup_deadline;
        while (session_current(session) && esp_timer_get_time() < response_deadline)
            vTaskDelay(pdMS_TO_TICKS(READ_TIMEOUT_MS));
        publish(session->generation, LINX_VOICE_WIFI_SAVED, LINX_VOICE_OK, NULL);
        break;
    }
done:
    clear_sensitive(&previous, sizeof(previous));
    clear_sensitive(&event, sizeof(event));
    return result;
}

static linx_voice_error_t trusted_time(voice_session_t *session)
{
    _Static_assert(CONFIG_LWIP_SNTP_MAX_SERVERS >= 3, "SNTP requires three server slots");
    /* TLS 仍校验证书和主机名；没有可信的日期时不连接服务端。 */
    if (time(NULL) >= 1704067200) return LINX_VOICE_OK;
    if (!esp_sntp_enabled()) {
        esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "ntp.aliyun.com");
        esp_sntp_setservername(1, "ntp.tencent.com");
        esp_sntp_setservername(2, "pool.ntp.org");
        esp_sntp_init();
        session->own_sntp = true;
    }
    /* SDK 首次应答超时为 15 秒，另有最多 5 秒启动延迟；给备用服务器留出轮换时间。 */
    int64_t started = esp_timer_get_time();
    int64_t deadline = started + TIME_SYNC_TIMEOUT_MS * 1000LL;
    ESP_LOGI("linx_time", "Waiting for SNTP (%d ms)", TIME_SYNC_TIMEOUT_MS);
    while (session_current(session) && time(NULL) < 1704067200) {
        if (atomic_load(&session->disconnected)) return LINX_VOICE_WIFI_FAILED;
        if (esp_timer_get_time() >= deadline) {
            ESP_LOGW("linx_time", "SNTP timed out");
            return LINX_VOICE_TIME_FAILED;
        }
        vTaskDelay(pdMS_TO_TICKS(READ_TIMEOUT_MS));
    }
    if (session_current(session))
        ESP_LOGI("linx_time", "SNTP synchronized (%lld ms)", (long long)((esp_timer_get_time() - started) / 1000));
    return LINX_VOICE_OK;
}

static esp_err_t http_event(esp_http_client_event_t *event)
{
    voice_session_t *session = event->user_data;
    if (!session_current(session)) return ESP_FAIL;
    if (event->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    if (event->data_len < 0 || (size_t)event->data_len > LINX_MESSAGE_CAPACITY - session->http_used) {
        session->http_overflow = true;
        return ESP_FAIL;
    }
    if (event->data_len) memcpy(session->receiver.data + session->http_used, event->data, (size_t)event->data_len);
    session->http_used += (size_t)event->data_len;
    return ESP_OK;
}

static linx_voice_error_t gateway(voice_session_t *session)
{
    wifi_ap_record_t access_point = {0};
    esp_err_t result = esp_wifi_sta_get_ap_info(&access_point);
    if (result != ESP_OK) return sdk_error(result, LINX_VOICE_WIFI_FAILED);
    char ssid[33] = {0};
    memcpy(ssid, access_point.ssid, sizeof(ssid) - 1U);
    const esp_app_desc_t *app = esp_app_get_description();
    char sha256[65];
    esp_app_get_elf_sha256(sha256, sizeof(sha256));
    linx_device_info_t info = {.device_id = session->device_id, .client_id = session->client_id,
                                    .version = app->version, .elf_sha256 = sha256,
                                    .ssid = ssid, .rssi = access_point.rssi};
    if (!linx_build_ota(session->scratch, sizeof(session->scratch), &info)) return LINX_VOICE_PROTOCOL_FAILED;
    esp_http_client_config_t config = {.url = OTA_URL, .timeout_ms = CONNECT_TIMEOUT_MS,
                                      .crt_bundle_attach = esp_crt_bundle_attach,
                                      .disable_auto_redirect = true, .event_handler = http_event,
                                      .user_data = session};
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return LINX_VOICE_NO_MEMORY;
    result = esp_http_client_set_method(client, HTTP_METHOD_POST);
    if (result == ESP_OK) result = esp_http_client_set_header(client, "Content-Type", "application/json");
    if (result == ESP_OK) result = esp_http_client_set_header(client, "Activation-Version", "1");
    if (result == ESP_OK) result = esp_http_client_set_header(client, "Device-Id", session->device_id);
    if (result == ESP_OK) result = esp_http_client_set_header(client, "Client-Id", session->client_id);
    if (result == ESP_OK) result = esp_http_client_set_header(client, "User-Agent", CLIENT_AGENT);
    if (result == ESP_OK) result = esp_http_client_set_post_field(client, session->scratch, (int)strlen(session->scratch));
    if (result == ESP_OK && session_current(session)) result = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI("linx_voice", "gateway_http result=%d status=%d", (int)result, status);
    /* HTTPS 与 WebSocket 的 TLS 内存不同时驻留。令牌只留在当前会话内存。 */
    esp_http_client_cleanup(client);
    if (session->http_overflow) return LINX_VOICE_PROTOCOL_FAILED;
    if (result != ESP_OK || status != 200) return sdk_error(result, LINX_VOICE_NETWORK_FAILED);
    linx_gateway_result_t parsed = linx_parse_gateway((char *)session->receiver.data,
                                                                 session->http_used, &session->gateway);
    if (parsed == LINX_GATEWAY_INVALID) return LINX_VOICE_PROTOCOL_FAILED;
    if (parsed == LINX_GATEWAY_BINDING)
        publish(session->generation, LINX_VOICE_BINDING, LINX_VOICE_OK, session->gateway.activation_code);
    return LINX_VOICE_OK;
}

/* 只记录边界与数字错误；不恢复 SDK 握手日志，也不输出外部消息内容。 */
static linx_voice_error_t transport_failure(voice_session_t *session, const char *operation,
                                          int result, unsigned opcode, int payload_size, int close_code)
{
    const char *phase = !session->hello ? "hello" : session->draining ? "draining" :
                        session->waiting ? "waiting" : session->speaking ? "playing" :
                        session->turn_active ? "recording" : "idle";
    int tls_code = 0, tls_flags = 0;
    esp_tls_error_handle_t errors = esp_transport_get_error_handle(session->ws);
    int esp_code = errors ? esp_tls_get_and_clear_last_error(errors, &tls_code, &tls_flags) : ESP_FAIL;
    int socket_errno = esp_transport_get_errno(session->ws);
    ESP_LOGW("linx_voice", "transport_failure operation=%s phase=%s ret=%d opcode=%u payload=%d errno=%d esp=%d tls=%d flags=%d close=%d",
             operation, phase, result, opcode, payload_size, socket_errno, esp_code, tls_code, tls_flags, close_code);
    return LINX_VOICE_NETWORK_FAILED;
}

static linx_voice_error_t send_frame(voice_session_t *session, unsigned opcode, void *data, size_t size)
{
    if (!session_current(session) ||
        (session->turn_active && !session->draining && opcode != WS_TRANSPORT_OPCODES_PONG &&
         !current(session->generation))) return LINX_VOICE_OK;
    /* ESP-IDF 会原地 mask，data 必须是可写 RAM，不能传常量或再次复用旧消息。 */
    int sent = esp_transport_ws_send_raw(session->ws, (ws_transport_opcodes_t)(opcode | WS_TRANSPORT_OPCODES_FIN),
                                         data, (int)size, SEND_TIMEOUT_MS);
    return sent == (int)size ? LINX_VOICE_OK
        : transport_failure(session, "send", sent, opcode, (int)size, -1);
}

static linx_voice_error_t listen_start(voice_session_t *session)
{
    atomic_store(&finish_pending, false);
    if (!linx_build_listen_request(session->scratch, sizeof(session->scratch), session->session_id,
                                   session->gateway_request_id))
        return LINX_VOICE_PROTOCOL_FAILED;
    linx_voice_error_t result = send_frame(session, WS_TRANSPORT_OPCODES_TEXT, session->scratch, strlen(session->scratch));
    if (result == LINX_VOICE_OK && current(session->generation)) {
        session->waiting = false;
        session->speaking = false;
        ESP_LOGI("linx_voice", "capture=on");
        atomic_store(&listening, true);
        publish(session->generation, LINX_VOICE_LISTENING, LINX_VOICE_OK, NULL);
    }
    return result;
}

static linx_voice_error_t websocket_start(voice_session_t *session, const linx_reply_context_t *context)
{
    linx_endpoint_t endpoint;
    if (!linx_parse_endpoint(session->gateway.url, &endpoint)) return LINX_VOICE_PROTOCOL_FAILED;
    session->ssl = esp_transport_ssl_init();
    if (!session->ssl) return LINX_VOICE_NO_MEMORY;
    esp_transport_ssl_crt_bundle_attach(session->ssl, esp_crt_bundle_attach);
    session->ws_log_level = esp_log_level_get("transport_ws");
    session->ws_log_muted = true;
    /* SDK 的 ERROR 也可能打印带 Authorization 的握手头，因此整段关闭该 TAG。 */
    esp_log_level_set("transport_ws", ESP_LOG_NONE);
    session->ws = esp_transport_ws_init(session->ssl);
    if (!session->ws) return LINX_VOICE_NO_MEMORY;
    int length = snprintf(session->scratch, sizeof(session->scratch),
                          "Authorization: Bearer %s\r\nProtocol-Version: 1\r\nDevice-Id: %s\r\nClient-Id: %s\r\n",
                          session->gateway.token, session->device_id, session->client_id);
    if (length < 0 || (size_t)length >= sizeof(session->scratch)) return LINX_VOICE_PROTOCOL_FAILED;
    esp_transport_ws_config_t config = {.ws_path = endpoint.path, .user_agent = CLIENT_AGENT,
                                        .headers = session->scratch, .propagate_control_frames = true};
    esp_err_t result = esp_transport_ws_set_config(session->ws, &config);
    memset(session->scratch, 0, sizeof(session->scratch));
    memset(session->gateway.token, 0, sizeof(session->gateway.token));
    if (result != ESP_OK) return sdk_error(result, LINX_VOICE_NETWORK_FAILED);
    if (!session_current(session)) return LINX_VOICE_OK;
    if (esp_transport_connect(session->ws, endpoint.host, endpoint.port, CONNECT_TIMEOUT_MS) < 0)
        return LINX_VOICE_NETWORK_FAILED;
    if (!linx_build_hello(session->scratch, sizeof(session->scratch), context)) return LINX_VOICE_PROTOCOL_FAILED;
    linx_receiver_init(&session->receiver);
    session->last_activity = esp_timer_get_time();
    return send_frame(session, WS_TRANSPORT_OPCODES_TEXT, session->scratch, strlen(session->scratch));
}

static linx_voice_error_t audio_start(voice_session_t *session)
{
    esp_err_t result = bsp_audio_init();
    /* init 可能已唤醒部分外设，失败路径同样调用 sleep 收拢资源。 */
    session->audio_awake = true;
    if (result == ESP_OK) result = bsp_audio_wake();
    if (result == ESP_OK) result = bsp_audio_set_format(16000, 16, 1);
    if (result != ESP_OK) return sdk_error(result, LINX_VOICE_AUDIO_FAILED);
    bsp_audio_set_volume(50);
    return LINX_VOICE_OK;
}

static linx_voice_error_t play_packet(voice_session_t *session)
{
    if (!session->hello || !session->speaking) return LINX_VOICE_PROTOCOL_FAILED;
    size_t size = session->receiver.used;
    uint8_t *data = session->receiver.data;
    if (session->audio == LINX_AUDIO_OPUS) {
        esp_audio_dec_in_raw_t raw = {.buffer = data, .len = (uint32_t)size};
        esp_audio_dec_out_frame_t frame = {.buffer = session->pcm, .len = sizeof(session->pcm)};
        esp_audio_dec_info_t info = {0};
        esp_audio_err_t result = esp_opus_dec_decode(session->decoder, &raw, &frame, &info);
        if (result != ESP_AUDIO_ERR_OK)
            return result == ESP_AUDIO_ERR_MEM_LACK ? LINX_VOICE_NO_MEMORY : LINX_VOICE_AUDIO_FAILED;
        if (raw.consumed != size || frame.decoded_size == 0 || frame.decoded_size > sizeof(session->pcm) ||
            frame.decoded_size % 2U || info.sample_rate != 16000 || info.channel != 1 || info.bits_per_sample != 16)
            return LINX_VOICE_AUDIO_FAILED;
        data = frame.buffer;
        size = frame.decoded_size;
    } else if (size == 0 || size > sizeof(session->pcm) || size % 2U) {
        return LINX_VOICE_PROTOCOL_FAILED;
    }
    if (!current(session->generation)) return LINX_VOICE_OK;
    esp_err_t result = bsp_audio_write(data, size);
    return result == ESP_OK ? LINX_VOICE_OK : sdk_error(result, LINX_VOICE_AUDIO_FAILED);
}

static linx_voice_error_t finish_playback(voice_session_t *session, bool aborted)
{
    /* 被中止的回复没有完整播完，不能当作本条指令的完成回执。 */
    if (aborted) return LINX_VOICE_PROTOCOL_FAILED;
    /* 固定上游 TX 为 6×240 采样，16 kHz 最多约90 ms，留120 ms播完尾音。 */
    for (unsigned i = 0; i < 6U; ++i) {
        if (!current(session->generation)) return LINX_VOICE_OK;
        if (atomic_load(&session->disconnected)) return LINX_VOICE_WIFI_FAILED;
        vTaskDelay(pdMS_TO_TICKS(READ_TIMEOUT_MS));
    }
    if (!current(session->generation)) return LINX_VOICE_OK;
    if (atomic_load(&session->disconnected)) return LINX_VOICE_WIFI_FAILED;
    session->speaking = false;
    session->completed = true;
    return LINX_VOICE_OK;
}

static linx_voice_error_t message(voice_session_t *session)
{
    if (atomic_load(&cancel_turn_pending) && session->turn_active && !session->drain_stopped)
        session->draining = true;
    if (session->receiver.kind == WS_TRANSPORT_OPCODES_BINARY)
        return session->draining || atomic_load(&cancel_turn_pending) ? LINX_VOICE_OK : play_packet(session);
    linx_message_t parsed;
    if (!linx_parse_message((char *)session->receiver.data, session->receiver.used, &parsed))
        return LINX_VOICE_PROTOCOL_FAILED;
    if (!session->hello) {
        if (parsed.kind != LINX_HELLO) return LINX_VOICE_PROTOCOL_FAILED;
        session->hello = true;
        session->audio = parsed.audio;
        memcpy(session->session_id, parsed.session_id, sizeof(session->session_id));
        return LINX_VOICE_OK;
    }
    if (parsed.kind == LINX_HELLO) return LINX_VOICE_PROTOCOL_FAILED;
    if (parsed.session_id[0] && strcmp(parsed.session_id, session->session_id)) return LINX_VOICE_PROTOCOL_FAILED;
    if (session->draining || atomic_load(&cancel_turn_pending)) {
        if (parsed.kind == LINX_TTS_STOP) {
            session->draining = false;
            session->drain_stopped = true;
            ESP_LOGI("linx_voice", "drain=complete barrier=tts_stop");
        }
        return LINX_VOICE_OK;
    }
    /* 无取消屏障的主动语音不能成为新一条指令的回复。 */
    if (!session->turn_active && parsed.kind != LINX_IGNORE) return LINX_VOICE_PROTOCOL_FAILED;
    if (parsed.kind == LINX_TTS_START) {
        /* 未结束上传前的主动问候不是本条指令的回复，不允许据此结束页面。 */
        if (!session->submitted || session->speaking) return LINX_VOICE_PROTOCOL_FAILED;
        atomic_store(&listening, false);
        atomic_store(&finish_pending, false);
        session->waiting = false;
        session->speaking = true;
        if (session->decoder && esp_opus_dec_reset(session->decoder) != ESP_AUDIO_ERR_OK) return LINX_VOICE_AUDIO_FAILED;
        publish(session->generation, LINX_VOICE_SPEAKING, LINX_VOICE_OK, NULL);
    } else if (parsed.kind == LINX_TTS_STOP) {
        if (!session->speaking) return LINX_VOICE_PROTOCOL_FAILED;
        session->drain_stopped = true;
        return finish_playback(session, parsed.aborted);
    }
    return LINX_VOICE_OK;
}

static linx_voice_error_t receive(voice_session_t *session)
{
    /* 短轮询保证录音帧节奏；已开始的帧允许正常 TCP 分段间隔。 */
    int ready = esp_transport_poll_read(session->ws, atomic_load(&listening) ? 0 : READ_TIMEOUT_MS);
    if (ready < 0)
        return transport_failure(session, "poll_read", ready, (unsigned)esp_transport_ws_get_read_opcode(session->ws),
                                 esp_transport_ws_get_read_payload_len(session->ws), -1);
    if (ready == 0) return LINX_VOICE_OK;
    int size = esp_transport_read(session->ws, session->scratch, LINX_MESSAGE_CAPACITY, FRAME_TIMEOUT_MS);
    unsigned opcode = (unsigned)esp_transport_ws_get_read_opcode(session->ws);
    int payload_size = esp_transport_ws_get_read_payload_len(session->ws);
    if (size < 0) return transport_failure(session, "read", size, opcode, payload_size, -1);
    if (!session_current(session)) return LINX_VOICE_OK;
    if (size == 0 && opcode == WS_TRANSPORT_OPCODES_NONE)
        return transport_failure(session, "read", size, opcode, payload_size, -1);
    if (payload_size < 0) return LINX_VOICE_PROTOCOL_FAILED;
    if ((opcode == WS_TRANSPORT_OPCODES_PING || opcode == WS_TRANSPORT_OPCODES_PONG) && payload_size > 125)
        return LINX_VOICE_PROTOCOL_FAILED;
    if (size == 0 && payload_size > 0) return transport_failure(session, "read", size, opcode, payload_size, -1);
    if (opcode == WS_TRANSPORT_OPCODES_CLOSE) {
        int code = size >= 2 ? ((unsigned char)session->scratch[0] << 8) | (unsigned char)session->scratch[1] : -1;
        return transport_failure(session, "close", size, opcode, payload_size, code);
    }
    if (opcode == WS_TRANSPORT_OPCODES_PING || opcode == WS_TRANSPORT_OPCODES_PONG) {
        bool complete;
        if (!linx_receive(&session->receiver, opcode, esp_transport_ws_get_fin_flag(session->ws),
                                (size_t)payload_size, session->scratch, (size_t)size, &complete))
            return LINX_VOICE_PROTOCOL_FAILED;
        if (size) memcpy(session->control + session->control_used, session->scratch, (size_t)size);
        session->control_used += (size_t)size;
        if (session->receiver.control_remaining) return LINX_VOICE_OK;
        linx_voice_error_t result = opcode == WS_TRANSPORT_OPCODES_PING
            ? send_frame(session, WS_TRANSPORT_OPCODES_PONG, session->control, session->control_used)
            : LINX_VOICE_OK;
        session->control_used = 0;
        return result;
    }
    if (!session->receiver.frame_active) {
        session->frame_opcode = opcode;
        session->frame_fin = esp_transport_ws_get_fin_flag(session->ws);
        session->frame_size = (size_t)payload_size;
    }
    bool complete = false;
    if (!linx_receive(&session->receiver, session->frame_opcode, session->frame_fin,
                             session->frame_size, session->scratch, (size_t)size, &complete))
        return LINX_VOICE_PROTOCOL_FAILED;
    if (!complete) return LINX_VOICE_OK;
    session->last_activity = esp_timer_get_time();
    return message(session);
}

static linx_voice_error_t conversation(voice_session_t *session)
{
    while (current(session->generation)) {
        if (atomic_load(&session->disconnected)) return LINX_VOICE_WIFI_FAILED;
        if (atomic_load(&listening)) {
            if (atomic_exchange(&finish_pending, false)) {
                atomic_store(&listening, false);
                ESP_LOGI("linx_voice", "capture=off");
                if (!linx_build_listen(session->scratch, sizeof(session->scratch), session->session_id, false))
                    return LINX_VOICE_PROTOCOL_FAILED;
                linx_voice_error_t result = send_frame(session, WS_TRANSPORT_OPCODES_TEXT,
                                                      session->scratch, strlen(session->scratch));
                if (result != LINX_VOICE_OK) return result;
                session->waiting = true;
                session->submitted = true;
                session->last_activity = esp_timer_get_time();
                publish(session->generation, LINX_VOICE_WAITING, LINX_VOICE_OK, NULL);
            } else {
                esp_err_t result = bsp_audio_read(session->microphone, sizeof(session->microphone));
                if (result != ESP_OK) return sdk_error(result, LINX_VOICE_AUDIO_FAILED);
                linx_voice_error_t sent = send_frame(session, WS_TRANSPORT_OPCODES_BINARY,
                                                    session->microphone, sizeof(session->microphone));
                if (sent != LINX_VOICE_OK) return sent;
            }
        }
        linx_voice_error_t result = receive(session);
        if (result != LINX_VOICE_OK) return result;
        if (session->completed) return LINX_VOICE_OK;
        int64_t elapsed = esp_timer_get_time() - session->last_activity;
        if ((!session->hello && elapsed >= READY_TIMEOUT_MS * 1000LL) ||
            ((session->waiting || session->speaking) && elapsed >= RESPONSE_TIMEOUT_MS * 1000LL))
            return LINX_VOICE_TIMEOUT;
    }
    return LINX_VOICE_OK;
}

/* 仅释放语音；STA 与其事件回调保留，任何异常都不会重传录音。 */
static linx_voice_error_t transport_reset(voice_session_t *session)
{
    linx_voice_error_t result = LINX_VOICE_OK;
    if (session->ws) {
        if (session->hello && session->turn_active && !session->completed &&
            linx_build_abort(session->scratch, sizeof(session->scratch), session->session_id))
            esp_transport_ws_send_raw(session->ws, WS_TRANSPORT_OPCODES_TEXT | WS_TRANSPORT_OPCODES_FIN,
                                      session->scratch, (int)strlen(session->scratch), READ_TIMEOUT_MS);
        esp_transport_close(session->ws);
        esp_transport_destroy(session->ws);
        session->ws = NULL;
    }
    if (session->ssl) esp_transport_destroy(session->ssl);
    session->ssl = NULL;
    if (session->ws_log_muted) esp_log_level_set("transport_ws", session->ws_log_level);
    session->ws_log_muted = false;
    if (session->decoder) esp_opus_dec_close(session->decoder);
    session->decoder = NULL;
    if (session->audio_awake) {
        esp_err_t slept = bsp_audio_sleep();
        session->audio_awake = slept != ESP_OK;
        if (slept != ESP_OK) result = LINX_VOICE_AUDIO_FAILED;
        ESP_LOGI("linx_voice", "capture=off audio_sleep_ok=%d", slept == ESP_OK);
    }
    if (session->own_sntp) esp_sntp_stop();
    session->own_sntp = false;
    clear_sensitive(&session->gateway, sizeof(session->gateway));
    clear_sensitive(&session->receiver, sizeof(session->receiver));
    clear_sensitive(session->session_id, sizeof(session->session_id));
    clear_sensitive(session->scratch, sizeof(session->scratch));
    clear_sensitive(session->pcm, sizeof(session->pcm));
    clear_sensitive(session->microphone, sizeof(session->microphone));
    clear_sensitive(session->control, sizeof(session->control));
    session->control_used = session->http_used = session->frame_size = 0;
    session->http_overflow = session->hello = session->speaking = session->waiting = false;
    session->submitted = session->completed = session->turn_active = session->frame_fin = false;
    session->preparing = session->draining = session->drain_stopped = false;
    session->frame_opcode = 0;
    session->last_activity = session->idle_deadline = 0;
    return result;
}

static linx_voice_error_t cleanup(voice_session_t *session)
{
    linx_voice_error_t result = LINX_VOICE_OK;
    if (retained == session && !network_close_wait()) return LINX_VOICE_TIMEOUT;
    /* 先关闭 HTTP 及其请求，再释放无线接口。 */
    if (session->portal) {
        esp_err_t stopped = linx_wifi_portal_stop(session->portal);
        if (stopped != ESP_OK) {
            /* 保留 HTTP 上下文及 netif；先关闭热点，再由同一 worker 重试停止。 */
            esp_err_t disabled = esp_wifi_set_storage(WIFI_STORAGE_RAM);
            if (disabled == ESP_OK) disabled = esp_wifi_set_mode(WIFI_MODE_STA);
            if (disabled != ESP_OK && session->wifi_started && esp_wifi_stop() == ESP_OK)
                session->wifi_started = false;
            pending_cleanup = session;
            return sdk_error(stopped, LINX_VOICE_WIFI_FAILED);
        }
        session->portal = NULL;
    }
    pending_cleanup = NULL;
    if (retained == session) retained = NULL;
    result = transport_reset(session);
    if (session->audio_awake) {
        /* 音频停服未确认时保留唯一所有者，配网也不能越过这个屏障。 */
        pending_cleanup = session;
        return result;
    }
    if (session->wifi_handler) esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, session->wifi_handler);
    if (session->ip_handler) esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, session->ip_handler);
    if (session->sync_handler) esp_event_handler_instance_unregister(LINX_WIFI_SYNC_EVENT, ESP_EVENT_ANY_ID, session->sync_handler);
    if (session->scan_started) {
        esp_wifi_scan_stop();
        if (esp_wifi_clear_ap_list() != ESP_OK) result = LINX_VOICE_WIFI_FAILED;
    }
    if (session->wifi_started && esp_wifi_stop() != ESP_OK) result = LINX_VOICE_WIFI_FAILED;
    if (session->wifi_initialized && esp_wifi_deinit() != ESP_OK) result = LINX_VOICE_WIFI_FAILED;
    if (session->ap_netif) esp_netif_destroy_default_wifi(session->ap_netif);
    if (session->netif) esp_netif_destroy_default_wifi(session->netif);
    if (session->own_loop && esp_event_loop_delete_default() != ESP_OK) result = LINX_VOICE_WIFI_FAILED;
    clear_sensitive(session, sizeof(*session));
    free(session);
    return result;
}


static linx_voice_error_t handshake(voice_session_t *session)
{
    while (session_current(session) && !session->hello) {
        if (atomic_load(&session->disconnected)) return LINX_VOICE_WIFI_FAILED;
        linx_voice_error_t result = receive(session);
        if (result != LINX_VOICE_OK) return result;
        if (!session->hello && esp_timer_get_time() - session->last_activity >= READY_TIMEOUT_MS * 1000LL)
            return LINX_VOICE_TIMEOUT;
    }
    return LINX_VOICE_OK;
}

/* 唤醒麦克风前先排空可见控制帧；迟到的语音/字幕让缓存失效。 */
static linx_voice_error_t idle_validate(voice_session_t *session)
{
    for (unsigned i = 0; i < 16U; ++i) {
        int ready = esp_transport_poll_read(session->ws, 0);
        if (ready < 0)
            return transport_failure(session, "poll_read", ready, (unsigned)esp_transport_ws_get_read_opcode(session->ws),
                                     esp_transport_ws_get_read_payload_len(session->ws), -1);
        if (ready == 0)
            return session->receiver.frame_active || session->receiver.control_remaining ||
                   (session->receiver.kind && !session->receiver.frame_fin)
                ? LINX_VOICE_PROTOCOL_FAILED : LINX_VOICE_OK;
        linx_voice_error_t result = receive(session);
        if (result != LINX_VOICE_OK) return result;
    }
    return LINX_VOICE_PROTOCOL_FAILED;
}

static linx_voice_error_t turn_start(voice_session_t *session)
{
    linx_voice_error_t result = audio_start(session);
    if (result != LINX_VOICE_OK) return result;
    if (session->audio == LINX_AUDIO_OPUS) {
        esp_opus_dec_cfg_t config = {.sample_rate = 16000, .channel = 1,
                                    .frame_duration = ESP_OPUS_DEC_FRAME_DURATION_120_MS,
                                    .self_delimited = false};
        esp_audio_err_t opened = esp_opus_dec_open(&config, sizeof(config), &session->decoder);
        if (opened != ESP_AUDIO_ERR_OK)
            return opened == ESP_AUDIO_ERR_MEM_LACK ? LINX_VOICE_NO_MEMORY : LINX_VOICE_AUDIO_FAILED;
    }
    session->completed = session->submitted = session->draining = session->drain_stopped = false;
    session->turn_active = true;
    session->idle_deadline = 0;
    return listen_start(session);
}

static linx_voice_error_t turn_sleep(voice_session_t *session)
{
    linx_voice_error_t result = LINX_VOICE_OK;
    if (session->decoder) esp_opus_dec_close(session->decoder);
    session->decoder = NULL;
    if (session->audio_awake) {
        esp_err_t slept = bsp_audio_sleep();
        session->audio_awake = slept != ESP_OK;
        if (slept != ESP_OK) result = LINX_VOICE_AUDIO_FAILED;
    }
    session->turn_active = session->submitted = session->waiting = session->speaking = false;
    clear_sensitive(session->pcm, sizeof(session->pcm));
    clear_sensitive(session->microphone, sizeof(session->microphone));
    ESP_LOGI("linx_voice", "capture=off audio_sleep_ok=%d", !session->audio_awake);
    return result;
}

static void renew_idle(voice_session_t *session)
{
    session->idle_deadline = esp_timer_get_time() + IDLE_TIMEOUT_MS * 1000LL;
    ESP_LOGI("linx_voice", "voice_lease=renewed ms=%d", IDLE_TIMEOUT_MS);
}

static linx_voice_error_t cancel_turn(voice_session_t *session)
{
    bool interrupted = session->turn_active && !session->completed;
    linx_voice_error_t result = LINX_VOICE_OK;
    if (interrupted && !session->drain_stopped && session->hello) {
        session->draining = true;
        if (!linx_build_abort(session->scratch, sizeof(session->scratch), session->session_id))
            result = LINX_VOICE_PROTOCOL_FAILED;
        else result = send_frame(session, WS_TRANSPORT_OPCODES_TEXT, session->scratch, strlen(session->scratch));
    }
    linx_voice_error_t slept = turn_sleep(session);
    if (result == LINX_VOICE_OK) result = slept;
    if (session->hello) renew_idle(session);
    ESP_LOGI("linx_voice", "cancel=soft transport_kept=%d draining=%d", session->ws != NULL, session->draining);
    return result;
}

static linx_voice_error_t wait_drain(voice_session_t *session)
{
    publish(session->generation, LINX_VOICE_DRAINING, LINX_VOICE_OK, NULL);
    int64_t deadline = esp_timer_get_time() + RESPONSE_TIMEOUT_MS * 1000LL;
    while (current(session->generation) && session->draining && !atomic_load(&discard_transport)) {
        if (atomic_load(&session->disconnected)) return LINX_VOICE_WIFI_FAILED;
        if (esp_timer_get_time() >= deadline || esp_timer_get_time() >= session->idle_deadline)
            return LINX_VOICE_TIMEOUT;
        linx_voice_error_t result = receive(session);
        if (result != LINX_VOICE_OK) return result;
    }
    return LINX_VOICE_OK;
}

static void run(const linx_voice_request_t *request)
{
    bool voice = request->command == LINX_VOICE_START || request->command == LINX_VOICE_PREPARE;
    if (!voice) {
        atomic_store(&network_provisioning, true);
        /* 配网前收拢业务连接；普通语音不拆除业务 TLS 或已有语音租约。 */
        if (!network_close_wait()) {
            atomic_store(&network_provisioning, false);
            publish(request->generation, LINX_VOICE_ERROR, LINX_VOICE_TIMEOUT, NULL);
            return;
        }
    }
    if (retained && (!voice || atomic_load(&retained->disconnected) || !atomic_load(&retained->connected))) {
        linx_voice_error_t cleaned = cleanup(retained);
        if (cleaned != LINX_VOICE_OK) {
            if (!voice) atomic_store(&network_provisioning, false);
            publish(request->generation, LINX_VOICE_ERROR, cleaned, NULL);
            return;
        }
    }
    voice_session_t *session = retained;
    if (!session) {
        session = calloc(1, sizeof(*session));
        if (!session) {
            if (!voice) atomic_store(&network_provisioning, false);
            publish(request->generation, LINX_VOICE_ERROR, LINX_VOICE_NO_MEMORY, NULL);
            return;
        }
    }
    session->generation = request->generation;
    session->requested_command = request->command;
    session->voice = voice;
    session->network_only = false;
    memcpy(session->gateway_request_id, request->gateway_request_id, sizeof(session->gateway_request_id));
    bool discard = atomic_exchange(&discard_transport, false);
    bool same_target = same_voice_target(&session->context, session->gateway_agent_session_id,
                                         &request->context, request->gateway_agent_session_id);
    bool reused = voice && session->hello && !session->turn_active && !session->audio_awake && !discard && same_target &&
                  esp_timer_get_time() < session->idle_deadline;
    linx_voice_error_t result = LINX_VOICE_OK;
    if (reused && !session->draining && idle_validate(session) != LINX_VOICE_OK) reused = false;
    if (voice && !reused) result = transport_reset(session);
    if (voice) {
        retained = session;
        session->context = request->context;
        memcpy(session->gateway_agent_session_id, request->gateway_agent_session_id, sizeof(session->gateway_agent_session_id));
        session->preparing = !reused;
        if (!reused) publish(session->generation, LINX_VOICE_CONNECTING, LINX_VOICE_OK, NULL);
        int64_t phase_start = esp_timer_get_time();
        bool wifi_reused = atomic_load(&session->connected);
        if (result == LINX_VOICE_OK && !wifi_reused) result = wifi_start(session);
        ESP_LOGI("linx_voice", "phase=wifi ms=%lld reused=%d",
                 (long long)((esp_timer_get_time() - phase_start) / 1000), wifi_reused);
        if (!reused && result == LINX_VOICE_OK && session_current(session)) result = trusted_time(session);
        if (session->own_sntp) esp_sntp_stop();
        session->own_sntp = false;
        phase_start = esp_timer_get_time();
        if (!reused && result == LINX_VOICE_OK && session_current(session)) result = gateway(session);
        ESP_LOGI("linx_voice", "phase=gateway ms=%lld reused=%d result=%d",
                 (long long)((esp_timer_get_time() - phase_start) / 1000), reused, (int)result);
        phase_start = esp_timer_get_time();
        if (!reused && result == LINX_VOICE_OK && session_current(session) && !session->gateway.activation_code[0])
            result = websocket_start(session, &session->context);
        if (!reused && result == LINX_VOICE_OK && session_current(session) && !session->gateway.activation_code[0])
            result = handshake(session);
        ESP_LOGI("linx_voice", "phase=voice_ready ms=%lld reused=%d",
                 (long long)((esp_timer_get_time() - phase_start) / 1000), reused);
        (void)phase_start;
        /* 这里完成最后一次同目标接管，之后本条采集严格检查页面代号。 */
        session_current(session);
        session->preparing = false;
        if (!reused && session->hello) renew_idle(session);
        if (atomic_exchange(&cancel_turn_pending, false)) {
            linx_voice_error_t cancelled = cancel_turn(session);
            if (result == LINX_VOICE_OK) result = cancelled;
        }
        linx_voice_command_t command = session->requested_command;
        if (result == LINX_VOICE_OK && current(session->generation) && session->hello) {
            if (command == LINX_VOICE_PREPARE) {
                publish(session->generation, session->draining ? LINX_VOICE_DRAINING : LINX_VOICE_READY, LINX_VOICE_OK, NULL);
            } else {
                /* 明确的新 START 续期；只有真正开始采集才暂停空闲倒计时。 */
                renew_idle(session);
                if (session->draining) result = wait_drain(session);
                if (result == LINX_VOICE_OK && current(session->generation) && !session->draining &&
                    !atomic_load(&discard_transport)) {
                    publish(session->generation, LINX_VOICE_PREPARING, LINX_VOICE_OK, NULL);
                    result = turn_start(session);
                    if (result == LINX_VOICE_OK && current(session->generation)) result = conversation(session);
                }
            }
        }
        if (current(session->generation)) {
            atomic_store(&listening, false);
            atomic_store(&finish_pending, false);
        }
        bool completed = command == LINX_VOICE_START && session->completed && current(session->generation);
        if (result == LINX_VOICE_OK && completed) result = turn_sleep(session);
        bool cancelled = atomic_exchange(&cancel_turn_pending, false);
        bool hard = atomic_exchange(&discard_transport, false);
        if (result == LINX_VOICE_OK && (cancelled || (session->turn_active && !current(session->generation))) && !hard)
            result = cancel_turn(session);
        /* 收尾未到 stop 的超时只报告，不偷偷关闭仍在租期内的连接。 */
        bool waiting_timeout = result == LINX_VOICE_TIMEOUT && session->draining;
        if ((result != LINX_VOICE_OK && !waiting_timeout) || session->turn_active || !session->hello || hard) {
            linx_voice_error_t cleaned = transport_reset(session);
            if (result == LINX_VOICE_OK) result = cleaned;
        }
        uint32_t generation = session->generation;
        session->handled_generation = generation;
        if (atomic_load(&session->disconnected) || !atomic_load(&session->connected)) {
            linx_voice_error_t cleaned = cleanup(session);
            if (result == LINX_VOICE_OK) result = cleaned;
        } else if (completed && result == LINX_VOICE_OK) renew_idle(session);
        if (result != LINX_VOICE_OK) publish(generation, LINX_VOICE_ERROR, result, NULL);
        else if (completed) publish(generation, LINX_VOICE_FINISHED, LINX_VOICE_OK, NULL);
        return;
    }
    if (request->command == LINX_VOICE_WIFI_SCAN) {
        publish(session->generation, LINX_VOICE_WIFI_SCANNING, LINX_VOICE_OK, NULL);
        result = wifi_scan(session);
    } else if (request->command == LINX_VOICE_WIFI_SETUP) {
        publish(session->generation, LINX_VOICE_WIFI_SCANNING, LINX_VOICE_OK, NULL);
        result = wifi_setup(session);
    } else {
        publish(session->generation, LINX_VOICE_WIFI_CONNECTING, LINX_VOICE_OK, NULL);
        result = wifi_save(session, request);
    }
    linx_voice_error_t cleaned = cleanup(session);
    if (result == LINX_VOICE_OK) result = cleaned;
    atomic_store(&network_provisioning, false);
    if (result != LINX_VOICE_OK) publish(request->generation, LINX_VOICE_ERROR, result, NULL);
}

static void idle_poll(void)
{
    voice_session_t *session = retained;
    if (!session) return;
    bool expired = session->hello && session->idle_deadline && esp_timer_get_time() >= session->idle_deadline;
    bool discard = atomic_exchange(&discard_transport, false);
    bool cancel = atomic_exchange(&cancel_turn_pending, false);
    if (expired) ESP_LOGI("linx_voice", "voice_idle_expired ms=%d", IDLE_TIMEOUT_MS);
    linx_voice_error_t result = LINX_VOICE_OK;
    if (discard || expired || session->audio_awake) result = transport_reset(session);
    else if (cancel) result = cancel_turn(session);
    else if (session->ws) {
        bool draining = session->draining;
        result = receive(session);
        if (draining && !session->draining)
            publish(session->generation, LINX_VOICE_READY, LINX_VOICE_OK, NULL);
    }
    if (result != LINX_VOICE_OK) {
        linx_voice_error_t cleaned = transport_reset(session);
        if (cleaned != LINX_VOICE_OK) publish(session->generation, LINX_VOICE_ERROR, cleaned, NULL);
    }
    if (atomic_load(&session->disconnected)) cleanup(session);
}

/* 与语音共用 retained 的唯一 owner；Gateway 只能读取独立快照。 */
static void network_tick(void)
{
    if (pending_cleanup || atomic_load(&network_provisioning)) return;
    if (!atomic_load(&network_wanted)) {
        if (retained && retained->network_only) cleanup(retained);
        return;
    }
    if (esp_timer_get_time() < network_retry_at) return;
    linx_voice_error_t error = LINX_VOICE_OK;
    if (retained && (atomic_load(&retained->disconnected) || !atomic_load(&retained->connected))) {
        error = cleanup(retained);
        if (retained || pending_cleanup) return;
    }
    if (!retained) {
        retained = calloc(1, sizeof(*retained));
        if (!retained) error = LINX_VOICE_NO_MEMORY;
        else {
            retained->network_only = true;
            error = wifi_start(retained);
        }
    }
    if (error == LINX_VOICE_OK && retained && !atomic_load(&network_ready)) {
        if (atomic_load(&network_close_ack) != atomic_load(&network_epoch)) return;
        error = trusted_time(retained);
        if (retained->own_sntp) esp_sntp_stop();
        retained->own_sntp = false;
        if (error == LINX_VOICE_OK && atomic_load(&retained->connected) &&
            !atomic_load(&retained->disconnected) && !retained->trial && !retained->portal &&
            atomic_load(&network_wanted)) {
            taskENTER_CRITICAL(&wifi_state_lock);
            memcpy(network_mac, retained->device_id, sizeof(network_mac));
            taskEXIT_CRITICAL(&wifi_state_lock);
            atomic_store(&network_ready, true);
        }
    }
    atomic_store(&network_error, error);
    if (error != LINX_VOICE_OK) {
        if (retained) cleanup(retained);
        network_retry_at = esp_timer_get_time() + 1000000LL;
    }
}

static void worker(void *user)
{
    (void)user;
    linx_voice_request_t request;
    for (;;) {
        if (pending_cleanup) {
            linx_voice_error_t cleaned = cleanup(pending_cleanup);
            if (cleaned != LINX_VOICE_OK && pending_cleanup) {
                publish(atomic_load(&active_generation), LINX_VOICE_ERROR, cleaned, NULL);
                vTaskDelay(pdMS_TO_TICKS(READ_TIMEOUT_MS));
                continue;
            }
        }
        network_tick();
        /* 有界等待也允许稍后 acquire 唤醒网络，无需占用 START 单槽。 */
        TickType_t wait = pdMS_TO_TICKS(READ_TIMEOUT_MS);
        if (xQueueReceive(commands, &request, wait) == pdTRUE) {
            bool starts = request.command == LINX_VOICE_START || request.command == LINX_VOICE_PREPARE ||
                          request.command == LINX_VOICE_WIFI_SCAN || request.command == LINX_VOICE_WIFI_SAVE ||
                          request.command == LINX_VOICE_WIFI_SETUP;
            bool adopted = retained && retained->voice && retained->handled_generation == request.generation &&
                           (request.command == LINX_VOICE_START || request.command == LINX_VOICE_PREPARE) &&
                           same_voice_target(&retained->context, retained->gateway_agent_session_id,
                                             &request.context, request.gateway_agent_session_id);
            if (starts && current(request.generation) && !adopted) run(&request);
            clear_sensitive(&request, sizeof(request));
        } else idle_poll();
        if (atomic_load(&discard_transport) || atomic_load(&cancel_turn_pending)) idle_poll();
    }
}

bool linx_device_init(void)
{
    if (commands) return true;
    QueueHandle_t command_queue = xQueueCreate(1, sizeof(linx_voice_request_t));
    QueueHandle_t event_queue = xQueueCreate(1, sizeof(linx_voice_event_t));
    if (!command_queue || !event_queue) {
        if (command_queue) vQueueDelete(command_queue);
        if (event_queue) vQueueDelete(event_queue);
        return false;
    }
    commands = command_queue;
    events = event_queue;
    if (xTaskCreate(worker, "linx_voice", WORKER_STACK_BYTES, NULL, 4, NULL) != pdPASS) {
        vQueueDelete(commands);
        vQueueDelete(events);
        commands = NULL;
        events = NULL;
        return false;
    }
    return true;
}

bool linx_device_request(const linx_voice_request_t *request, void *user)
{
    (void)user;
    if (!request || (unsigned)request->command > LINX_VOICE_RELEASE) return false;
    bool starts = request->command == LINX_VOICE_START || request->command == LINX_VOICE_WIFI_SCAN ||
                  request->command == LINX_VOICE_WIFI_SAVE || request->command == LINX_VOICE_WIFI_SETUP ||
                  request->command == LINX_VOICE_PREPARE;
    if (starts) {
        if (!memchr(request->gateway_agent_session_id, 0, sizeof(request->gateway_agent_session_id)) ||
            !memchr(request->gateway_request_id, 0, sizeof(request->gateway_request_id))) return false;
        bool gateway = request->gateway_agent_session_id[0] != 0;
        if ((gateway && !linx_protocol_id_valid(request->gateway_agent_session_id)) ||
            (request->gateway_request_id[0] && !linx_protocol_id_valid(request->gateway_request_id)) ||
            (!gateway && request->gateway_request_id[0]) ||
            (gateway && request->command == LINX_VOICE_START && !request->gateway_request_id[0])) return false;
    }
    if (!commands && (!starts || !linx_device_init())) return false;
    if (starts) {
        atomic_store(&listening, false);
        atomic_store(&finish_pending, false);
        atomic_store(&active, false);
        atomic_store(&active_generation, request->generation);
        taskENTER_CRITICAL(&command_state_lock);
        requested_target = (voice_target_t){.command = request->command, .generation = request->generation,
                                            .context = request->context};
        memcpy(requested_target.gateway_request_id, request->gateway_request_id, sizeof(requested_target.gateway_request_id));
        memcpy(requested_target.gateway_agent_session_id, request->gateway_agent_session_id, sizeof(requested_target.gateway_agent_session_id));
        taskEXIT_CRITICAL(&command_state_lock);
        atomic_store(&active, true);
        return xQueueOverwrite(commands, request) == pdTRUE;
    }
    if (!current(request->generation) &&
        !(request->command == LINX_VOICE_RELEASE && request->generation == atomic_load(&active_generation))) return true;
    if (request->command == LINX_VOICE_FINISH) {
        if (atomic_load(&listening)) atomic_store(&finish_pending, true);
        return true;
    }
    if (request->command == LINX_VOICE_RELEASE) atomic_store(&discard_transport, true);
    else atomic_store(&cancel_turn_pending, true);
    atomic_store(&active, false);
    atomic_store(&listening, false);
    atomic_store(&finish_pending, false);
    linx_voice_event_t event = {.generation = request->generation, .state = LINX_VOICE_CLOSED};
    xQueueOverwrite(events, &event);
    return xQueueOverwrite(commands, request) == pdTRUE;
}

bool linx_device_poll(linx_voice_event_t *event)
{
    if (!event || !events || xQueueReceive(events, event, 0) != pdTRUE) return false;
    return event->generation == atomic_load(&active_generation) &&
           (atomic_load(&active) || event->state == LINX_VOICE_CLOSED);
}

bool linx_device_network_acquire(void)
{
    if (!linx_device_init()) return false;
    atomic_store(&network_wanted, true);
    return true;
}

void linx_device_network_release(void)
{
    atomic_store(&network_wanted, false);
    network_invalidate();
}

bool linx_device_network_poll(linx_device_network_state_t *state)
{
    if (!state) return false;
    taskENTER_CRITICAL(&wifi_state_lock);
    *state = (linx_device_network_state_t){.ready = atomic_load(&network_ready),
        .epoch = atomic_load(&network_epoch), .provisioning = atomic_load(&network_provisioning),
        .error = (linx_voice_error_t)atomic_load(&network_error)};
    memcpy(state->mac_address, network_mac, sizeof(state->mac_address));
    taskEXIT_CRITICAL(&wifi_state_lock);
    return true;
}

void linx_device_network_closed(uint32_t epoch)
{
    if (epoch == atomic_load(&network_epoch)) atomic_store(&network_close_ack, epoch);
}
