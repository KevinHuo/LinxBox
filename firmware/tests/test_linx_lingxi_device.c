/* 用真实协议解析器驱动设备 worker；只有 ESP-IDF、Codec 和 BSP 边界被替换。 */
#include <assert.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sdk_stubs.h"

const char *test_ssid = "";
const char *test_password = "";
static time_t test_time(time_t *out);
static void *test_session_alloc(size_t count, size_t size);
static void test_session_free(void *data);
#define time test_time
#define calloc test_session_alloc
#define free test_session_free
#include "../hardware/folotoy/ai-passport/linx_lingxi_device.c"
#undef time
#undef calloc
#undef free

struct test_queue { size_t size; bool ready; uint8_t data[sizeof(linx_voice_request_t) + sizeof(linx_voice_event_t)]; };
struct test_http { esp_http_client_config_t config; bool live; };
struct test_transport { bool live; bool ssl; };
esp_event_base_t WIFI_EVENT = "wifi";
esp_event_base_t IP_EVENT = "ip";
static esp_netif_t mock_netif, mock_ap_netif;
static bool ap_netif_live, existing_ap_netif;
static int wifi_mode;
static unsigned ap_sets, ap_destroys, disconnects, connect_calls;
struct linx_wifi_portal {bool live;};
static struct linx_wifi_portal mock_portal;
static unsigned portal_starts, portal_stops, portal_polls, portal_statuses[4], portal_stop_failures;
static unsigned portal_submissions, portal_failures, portal_submission_count;
static unsigned cancel_delay_at;
static bool portal_replace, disconnect_returns_not_connected;
static bool portal_cancel, portal_disconnect_error, portal_time_out, portal_cancel_success;
static bool ignore_association, unrelated_association, old_ip_after_disconnect;
static struct test_http mock_http;
static struct test_transport mock_ssl, mock_ws;
static jmp_buf worker_end;
static void (*worker_function)(void *);
static const char *failure;
static int failure_code, failure_index, failure_calls;
static bool session_oom, existing_loop, existing_netif, wifi_auto_connect;
static bool wifi_live, wifi_running, loop_live, netif_live, codec_live, audio_live, audio_initialized;
static bool nvs_missing, nvs_invalid, configured, sntp_existing, sntp_running;
static bool cancel_delay, cancel_perform, cancel_decode, cancel_audio_read;
static int disconnect_delay, time_ready_delay;
static unsigned delay_calls;
static int64_t clock_us;
static time_t wall_time;
static char persisted_uuid[37];
static unsigned event_counts[LINX_VOICE_WIFI_PORTAL + 1], queue_calls, task_calls, session_allocations;
static unsigned wifi_stops, wifi_deinits, netif_destroys, loop_deletes, unregisters, nvs_closes;
static unsigned http_cleanups, ssl_destroys, ws_destroys, ws_closes, sntp_stops;
static unsigned sntp_starts;
static const char *sntp_servers[CONFIG_LWIP_SNTP_MAX_SERVERS];
static int sntp_response_server;
static int64_t sntp_started_us;
static unsigned audio_reads, audio_writes, audio_sleeps, opus_opens, opus_resets, opus_decodes, opus_closes;
static unsigned listen_starts, listen_stops, aborts, audio_sends, pongs;
static unsigned decode_fault;
static int http_status;
static const char *http_body;
static bool http_negative, http_overflow;
static bool poll_empty;
static int log_level;
static void (*wifi_callback)(void *, esp_event_base_t, int32_t, void *);
static void (*ip_callback)(void *, esp_event_base_t, int32_t, void *);
static void *callback_user;
static void (*sync_callback)(void *, esp_event_base_t, int32_t, void *);
static bool disconnect_pending_event, sync_pending_event, drop_disconnect, drop_barrier, drop_retry_disconnect;
static unsigned barrier_posts, barriers_dispatched, delayed_disconnects;
static wifi_event_sta_disconnected_t queued_disconnect;
static uint32_t queued_generation;
static int32_t queued_barrier;
static unsigned handler_count;
static wifi_config_t flash_config, ram_config;
static int wifi_storage;
static unsigned ram_sets, flash_sets, config_noops, cancel_ram_at;
static bool cancel_on_disconnect, flash_error_after_ram;
static bool cancel_on_flash, cancel_on_ram, cancel_on_connect, cancel_on_radio;
static bool scan_auto_complete, scan_live, scan_invalid_count, cancel_scan_results, replace_scan_with_save;
static unsigned scan_status, scan_clears, scan_stops, scan_calls;
static wifi_ap_record_t scan_records[SCAN_RECORD_CAPACITY];
static uint16_t scan_record_count;
static unsigned last_opcode;
static bool last_fin;
static int last_payload;

typedef struct {
    unsigned opcode;
    bool fin;
    int payload;
    const void *data;
    int size;
    unsigned action;
} frame_t;
static frame_t frames[40];
static unsigned frame_count, frame_index;
static const char *hello_opus = "{\"type\":\"hello\",\"transport\":\"websocket\",\"session_id\":\"session-1\",\"audio_params\":{\"format\":\"opus\",\"sample_rate\":24000,\"channels\":1}}";
static const char *hello_pcm = "{\"type\":\"hello\",\"transport\":\"websocket\",\"session_id\":\"session-1\",\"audio_params\":{\"format\":\"pcm\",\"sample_rate\":16000,\"channels\":1}}";
static const char *tts_start = "{\"type\":\"tts\",\"state\":\"start\",\"session_id\":\"session-1\"}";
static const char *tts_stop = "{\"type\":\"tts\",\"state\":\"stop\",\"session_id\":\"session-1\"}";

static esp_err_t injected(const char *operation)
{
    if (!failure || strcmp(failure, operation)) return ESP_OK;
    return ++failure_calls == failure_index ? failure_code : ESP_OK;
}
#define INJECT() do { esp_err_t injected_result = injected(__func__); if (injected_result != ESP_OK) return injected_result; } while (0)

void test_task_enter(portMUX_TYPE *lock) { assert(lock->depth == 0); ++lock->depth; }
void test_task_exit(portMUX_TYPE *lock) { assert(lock->depth == 1); --lock->depth; }

static void cancel(void)
{
    linx_voice_request_t request = {.command = LINX_VOICE_CANCEL, .generation = atomic_load(&active_generation)};
    assert(linx_lingxi_device_request(&request, NULL));
}

static void finish(void)
{
    linx_voice_request_t request = {.command = LINX_VOICE_FINISH, .generation = atomic_load(&active_generation)};
    assert(linx_lingxi_device_request(&request, NULL));
}

static void *test_session_alloc(size_t count, size_t size)
{
    assert(count == 1 && size == sizeof(voice_session_t));
    ++session_allocations;
    return session_oom ? NULL : calloc(count, size);
}

static void test_session_free(void *data)
{
    const uint8_t *bytes = data;
    for (size_t i = 0; i < sizeof(voice_session_t); ++i) assert(bytes[i] == 0);
    free(data);
}

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t size)
{
    ++queue_calls;
    assert(length == 1 && size <= sizeof(((struct test_queue *)0)->data));
    if (injected(__func__) != ESP_OK) return NULL;
    struct test_queue *queue = calloc(1, sizeof(*queue));
    assert(queue);
    queue->size = size;
    return queue;
}

void vQueueDelete(QueueHandle_t queue) { assert(queue); free(queue); }
BaseType_t xQueueOverwrite(QueueHandle_t queue, const void *data)
{
    assert(queue && data);
    memcpy(queue->data, data, queue->size);
    queue->ready = true;
    if (queue == events) {
        const linx_voice_event_t *event = data;
        assert(event->state <= LINX_VOICE_WIFI_PORTAL);
        ++event_counts[event->state];
    }
    return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t queue, void *data, TickType_t wait)
{
    assert(queue && data);
    if (!queue->ready) {
        if (wait == portMAX_DELAY) longjmp(worker_end, 1);
        assert(wait == 0);
        return pdFALSE;
    }
    memcpy(data, queue->data, queue->size);
    queue->ready = false;
    return pdTRUE;
}
BaseType_t xTaskCreate(void (*function)(void *), const char *name, uint32_t stack, void *user, UBaseType_t priority, void *handle)
{
    ++task_calls;
    assert(!strcmp(name, "linx_voice") && stack == 16384 && user == NULL && priority == 4 && handle == NULL);
    worker_function = function;
    return injected(__func__) == ESP_OK ? pdPASS : pdFALSE;
}
void vTaskDelay(TickType_t ticks)
{
    assert(ticks == READ_TIMEOUT_MS);
    ++delay_calls;
    clock_us += ticks * 1000;
    if (time_ready_delay && delay_calls >= (unsigned)time_ready_delay) wall_time = 1800000000;
    /* 模拟 SDK 的启动延迟和首台无应答后 15 秒才轮换，不依赖应用的超时常量。 */
    if (sntp_running && sntp_response_server >= 0 && sntp_servers[sntp_response_server] &&
        clock_us - sntp_started_us >= (5000 + 15000 * sntp_response_server + 20) * 1000LL)
        wall_time = 1800000000;
    if (disconnect_delay && delay_calls >= (unsigned)disconnect_delay) wifi_callback(callback_user, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, NULL);
    if (scan_live && replace_scan_with_save) {
        replace_scan_with_save = false;
        linx_voice_request_t replacement = {.command = LINX_VOICE_WIFI_SAVE, .generation = 8};
        strcpy(replacement.wifi_network.ssid, "next-network");
        replacement.wifi_network.security = LINX_WIFI_PERSONAL;
        strcpy(replacement.wifi_password, "next-secret");
        assert(linx_lingxi_device_request(&replacement, NULL));
        memset(&replacement, 0, sizeof(replacement));
    }
    if (scan_live && scan_auto_complete) {
        wifi_event_sta_scan_done_t done = {.status = scan_status};
        wifi_callback(callback_user, WIFI_EVENT, WIFI_EVENT_SCAN_DONE, &done);
    }
    if (disconnect_pending_event && !drop_disconnect) {
        disconnect_pending_event = false; ++delayed_disconnects;
        wifi_callback(callback_user, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &queued_disconnect);
        if (old_ip_after_disconnect) ip_callback(callback_user, IP_EVENT, IP_EVENT_STA_GOT_IP, NULL);
    }
    if (sync_pending_event && !disconnect_pending_event && !drop_barrier) {
        sync_pending_event = false; ++barriers_dispatched;
        sync_callback(callback_user, LINX_WIFI_SYNC_EVENT, queued_barrier, &queued_generation);
    }
    if (cancel_delay || (cancel_delay_at && delay_calls >= cancel_delay_at)) cancel();
}
int64_t esp_timer_get_time(void) { return clock_us; }
static time_t test_time(time_t *out) { if (out) *out = wall_time; return wall_time; }

esp_err_t nvs_flash_init(void) { INJECT(); return ESP_OK; }
esp_err_t esp_read_mac(uint8_t *mac, int type) { assert(type == ESP_MAC_WIFI_STA); INJECT(); memset(mac, 0xab, 6); return ESP_OK; }
void esp_fill_random(void *buffer, size_t size) { assert(size == 16 || size == 8); memset(buffer, 0x3a, size); }
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle) { assert(!strcmp(name, "linx") && mode == NVS_READWRITE); INJECT(); *handle = 1; return ESP_OK; }
esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *value, size_t *size)
{
    assert(handle == 1 && !strcmp(key, "client_id") && *size == 37); INJECT();
    if (nvs_missing) return ESP_ERR_NVS_NOT_FOUND;
    strcpy(value, persisted_uuid); *size = nvs_invalid ? 4 : 37; return ESP_OK;
}
esp_err_t nvs_set_str(nvs_handle_t handle, const char *key, const char *value)
{
    assert(handle == 1 && !strcmp(key, "client_id")); INJECT();
    assert(strlen(value) == 36 && value[14] == '4' && strchr("89ab", value[19]));
    strcpy(persisted_uuid, value); return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle) { assert(handle == 1); INJECT(); return ESP_OK; }
void nvs_close(nvs_handle_t handle) { assert(handle == 1); ++nvs_closes; }
esp_err_t esp_netif_init(void) { INJECT(); return ESP_OK; }
esp_err_t esp_event_post(esp_event_base_t base, int32_t id, const void *data, size_t size, TickType_t wait)
{
    assert(base == LINX_WIFI_SYNC_EVENT && size == sizeof(uint32_t) && wait == 0); INJECT();
    assert(!sync_pending_event); sync_pending_event = true;
    queued_barrier = id; memcpy(&queued_generation, data, size); ++barrier_posts;
    return ESP_OK;
}
esp_err_t esp_event_loop_create_default(void) { INJECT(); if (existing_loop) return ESP_ERR_INVALID_STATE; loop_live = true; return ESP_OK; }
esp_err_t esp_event_loop_delete_default(void) { assert(loop_live); loop_live = false; ++loop_deletes; INJECT(); return ESP_OK; }
esp_netif_t *esp_netif_get_handle_from_ifkey(const char *key) { if (!strcmp(key, "WIFI_AP_DEF")) return existing_ap_netif ? &mock_ap_netif : NULL; assert(!strcmp(key, "WIFI_STA_DEF")); return existing_netif ? &mock_netif : NULL; }
esp_netif_t *esp_netif_new(const esp_netif_config_t *config) { assert(config); if (injected(__func__) != ESP_OK) return NULL; if (config->value) { assert(!ap_netif_live); ap_netif_live = true; return &mock_ap_netif; } assert(!netif_live); netif_live = true; return &mock_netif; }
esp_err_t esp_netif_attach_wifi_station(esp_netif_t *netif) { assert(netif == &mock_netif); INJECT(); return ESP_OK; }
esp_err_t esp_netif_attach_wifi_ap(esp_netif_t *netif) { assert(netif == &mock_ap_netif); INJECT(); return ESP_OK; }
esp_err_t esp_wifi_set_default_wifi_ap_handlers(void) { INJECT(); return ESP_OK; }
esp_err_t esp_wifi_set_default_wifi_sta_handlers(void) { INJECT(); return ESP_OK; }
void esp_netif_destroy_default_wifi(void *netif) { assert(!wifi_live && !mock_portal.live); if (netif == &mock_ap_netif) { assert(ap_netif_live); ap_netif_live = false; ++ap_destroys; } else { assert(netif == &mock_netif && netif_live); netif_live = false; ++netif_destroys; } }
esp_err_t esp_wifi_init(const wifi_init_config_t *config) {
    assert(config); INJECT(); wifi_live = true;
    if (!configured) memset(&flash_config, 0, sizeof(flash_config));
    ram_config = flash_config; wifi_storage = WIFI_STORAGE_FLASH;
    return ESP_OK;
}
esp_err_t esp_wifi_deinit(void) { assert(wifi_live && !wifi_running); wifi_live = false; memset(&ram_config, 0, sizeof(ram_config)); ++wifi_deinits; INJECT(); return ESP_OK; }
esp_err_t esp_event_handler_instance_register(esp_event_base_t base, int32_t id, void (*handler)(void *, esp_event_base_t, int32_t, void *), void *user, esp_event_handler_instance_t *instance)
{
    INJECT();
    if (base == WIFI_EVENT) { assert(id == ESP_EVENT_ANY_ID); wifi_callback = handler; }
    else if (base == IP_EVENT) { assert(id == IP_EVENT_STA_GOT_IP); ip_callback = handler; }
    else { assert(base == LINX_WIFI_SYNC_EVENT && id == ESP_EVENT_ANY_ID); sync_callback = handler; }
    callback_user = user;
    *instance = (void *)(uintptr_t)(++handler_count);
    return ESP_OK;
}
esp_err_t esp_event_handler_instance_unregister(esp_event_base_t base, int32_t id, esp_event_handler_instance_t instance)
{
    assert(instance);
    if (base == WIFI_EVENT || base == LINX_WIFI_SYNC_EVENT) assert(id == ESP_EVENT_ANY_ID); else assert(base == IP_EVENT && id == IP_EVENT_STA_GOT_IP);
    ++unregisters; return ESP_OK;
}
esp_err_t esp_wifi_set_storage(int storage) { assert(storage == WIFI_STORAGE_FLASH || storage == WIFI_STORAGE_RAM); INJECT(); wifi_storage = storage; return ESP_OK; }
esp_err_t esp_wifi_set_mode(int mode) { assert(mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA); INJECT(); wifi_mode = mode; return ESP_OK; }
esp_err_t esp_wifi_get_config(int interface, wifi_config_t *config) { assert(interface == WIFI_IF_STA); INJECT(); *config = ram_config; return ESP_OK; }
esp_err_t esp_wifi_set_config(int interface, wifi_config_t *config) {
    assert(interface == WIFI_IF_STA || interface == WIFI_IF_AP); INJECT();
    if (interface == WIFI_IF_AP) {
        assert(wifi_storage == WIFI_STORAGE_RAM && wifi_mode == WIFI_MODE_APSTA && ap_netif_live && !wifi_running);
        assert(config->ap.authmode == WIFI_AUTH_WPA2_PSK && config->ap.max_connection == 2);
        assert(config->ap.ssid_len == 14 && !memcmp(config->ap.ssid, "LinxBox-ABABAB", 14));
        for (unsigned i = 0; i < 8; ++i) assert(config->ap.password[i] >= '0' && config->ap.password[i] <= '9');
        ++ap_sets; return ESP_OK;
    }
    /* 与真实 C3 SDK 一致：规范化后的 RAM 配置相同会提前返回，不 flush NVS。 */
    if (!memcmp(&ram_config.sta, &config->sta, sizeof(config->sta))) { ++config_noops; return ESP_OK; }
    ram_config = *config;
    if (wifi_storage == WIFI_STORAGE_FLASH) {
        if (flash_error_after_ram) { flash_error_after_ram = false; return ESP_FAIL; }
        flash_config = *config; configured = config->sta.ssid[0] != 0; ++flash_sets;
        if (cancel_on_flash) { cancel_on_flash = false; cancel(); }
    } else {
        ++ram_sets;
        if (cancel_on_ram || (cancel_ram_at && ram_sets == cancel_ram_at)) cancel();
    }
    return ESP_OK;
}
esp_err_t esp_wifi_start(void) { assert(wifi_live); INJECT(); wifi_running = true; if (cancel_on_radio) cancel(); return ESP_OK; }
esp_err_t esp_wifi_stop(void) { assert(wifi_running); wifi_running = false; ++wifi_stops; INJECT(); return ESP_OK; }
esp_err_t esp_wifi_connect(void) { assert(wifi_running); INJECT(); if (cancel_on_connect) cancel(); ++connect_calls;
    if (ap_netif_live) assert(!disconnect_pending_event && !sync_pending_event && barrier_posts == barriers_dispatched);
    if (wifi_auto_connect) {
        wifi_event_sta_connected_t connected = {0};
        memcpy(connected.ssid, ram_config.sta.ssid, 32);
        while (connected.ssid_len < 32 && ram_config.sta.ssid[connected.ssid_len]) ++connected.ssid_len;
        if (unrelated_association) { memcpy(connected.ssid, "unrelated", 9); connected.ssid_len = 9; }
        if (!ignore_association) wifi_callback(callback_user, WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, &connected);
        ip_callback(callback_user, IP_EVENT, IP_EVENT_STA_GOT_IP, NULL);
    }
    return ESP_OK; }
esp_err_t esp_wifi_disconnect(void) {
    INJECT(); ++disconnects;
    if (cancel_on_disconnect) cancel();
    if (disconnect_returns_not_connected) return ESP_ERR_WIFI_NOT_CONNECT;
    assert(!disconnect_pending_event);
    memset(&queued_disconnect, 0, sizeof(queued_disconnect));
    memcpy(queued_disconnect.ssid, ram_config.sta.ssid, sizeof(queued_disconnect.ssid));
    while (queued_disconnect.ssid_len < 32 && ram_config.sta.ssid[queued_disconnect.ssid_len]) ++queued_disconnect.ssid_len;
    disconnect_pending_event = true;
    return ESP_OK;
}
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *record) { INJECT(); strcpy((char *)record->ssid, "existing-net"); record->rssi = -42; return ESP_OK; }
esp_err_t esp_wifi_scan_start(const wifi_scan_config_t *config, bool block)
{
    assert(wifi_running && config && !config->show_hidden && !block); INJECT();
    ++scan_calls; scan_live = true; return ESP_OK;
}
esp_err_t esp_wifi_scan_stop(void) { assert(scan_live); scan_live = false; ++scan_stops; INJECT(); return ESP_OK; }
esp_err_t esp_wifi_clear_ap_list(void) { ++scan_clears; INJECT(); return ESP_OK; }
esp_err_t esp_wifi_scan_get_ap_records(uint16_t *number, wifi_ap_record_t *records)
{
    assert(scan_live && *number == SCAN_RECORD_CAPACITY && records); INJECT();
    memcpy(records, scan_records, scan_record_count * sizeof(*records));
    *number = scan_invalid_count ? SCAN_RECORD_CAPACITY + 1 : scan_record_count;
    if (cancel_scan_results) cancel();
    return ESP_OK;
}
bool esp_sntp_enabled(void) { return sntp_existing; }
void esp_sntp_setoperatingmode(int mode) { assert(mode == ESP_SNTP_OPMODE_POLL); }
void esp_sntp_setservername(unsigned index, const char *name)
{
    assert(index < CONFIG_LWIP_SNTP_MAX_SERVERS && name);
    sntp_servers[index] = name;
}
void esp_sntp_init(void) { assert(!sntp_running); sntp_running = true; sntp_started_us = clock_us; ++sntp_starts; }
void esp_sntp_stop(void) { assert(sntp_running); sntp_running = false; ++sntp_stops; }
static const esp_app_desc_t app = {.version = "0.2"};
const esp_app_desc_t *esp_app_get_description(void) { return &app; }
int esp_app_get_elf_sha256(char *out, size_t size) { assert(size == 65); memset(out, '1', 64); out[64] = 0; return 64; }
esp_err_t esp_crt_bundle_attach(void *config) { (void)config; return ESP_OK; }
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config)
{
    assert(!strcmp(config->url, OTA_URL) && config->timeout_ms == 5000 && config->disable_auto_redirect);
    assert(config->crt_bundle_attach == esp_crt_bundle_attach && config->event_handler && config->user_data);
    if (injected(__func__) != ESP_OK) return NULL;
    assert(!mock_http.live && !mock_ssl.live);
    mock_http.config = *config; mock_http.live = true; return &mock_http;
}
esp_err_t esp_http_client_set_method(esp_http_client_handle_t client, int method) { assert(client == &mock_http && method == HTTP_METHOD_POST); INJECT(); return ESP_OK; }
esp_err_t esp_http_client_set_header(esp_http_client_handle_t client, const char *name, const char *value)
{
    assert(client == &mock_http && name && value); INJECT();
    if (!strcmp(name, "Activation-Version")) assert(!strcmp(value, "1"));
    if (!strcmp(name, "Device-Id")) assert(!strcmp(value, "ab:ab:ab:ab:ab:ab"));
    if (!strcmp(name, "Client-Id")) assert(strlen(value) == 36);
    return ESP_OK;
}
esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t client, const char *body, int size)
{
    assert(client == &mock_http && size == (int)strlen(body)); INJECT();
    assert(strstr(body, "mac_address") && strstr(body, "folotoy-ai-passport")); return ESP_OK;
}
esp_err_t esp_http_client_perform(esp_http_client_handle_t client)
{
    assert(client == &mock_http); INJECT();
    if (cancel_perform) cancel();
    esp_http_client_event_t event = {.event_id = 0, .user_data = client->config.user_data};
    if (client->config.event_handler(&event) != ESP_OK) return ESP_FAIL;
    event.event_id = HTTP_EVENT_ON_DATA;
    event.data = (void *)http_body;
    event.data_len = http_negative ? -1 : http_overflow ? 4097 : (int)strlen(http_body);
    if (http_overflow || http_negative) return client->config.event_handler(&event);
    int total = event.data_len; event.data_len = total / 2;
    if (client->config.event_handler(&event) != ESP_OK) return ESP_FAIL;
    event.data = (char *)event.data + event.data_len;
    event.data_len = total - event.data_len;
    return client->config.event_handler(&event);
}
int esp_http_client_get_status_code(esp_http_client_handle_t client) { assert(client == &mock_http); return http_status; }
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client) { assert(client == &mock_http && client->live); client->live = false; ++http_cleanups; return ESP_OK; }

esp_transport_handle_t esp_transport_ssl_init(void) { if (injected(__func__) != ESP_OK) return NULL; assert(!mock_http.live); mock_ssl = (struct test_transport){.live = true, .ssl = true}; return &mock_ssl; }
void esp_transport_ssl_crt_bundle_attach(esp_transport_handle_t transport, esp_err_t (*attach)(void *)) { assert(transport == &mock_ssl && attach == esp_crt_bundle_attach); }
esp_transport_handle_t esp_transport_ws_init(esp_transport_handle_t parent) { assert(parent == &mock_ssl && log_level == 0); if (injected(__func__) != ESP_OK) return NULL; mock_ws = (struct test_transport){.live = true}; return &mock_ws; }
esp_err_t esp_transport_ws_set_config(esp_transport_handle_t transport, const esp_transport_ws_config_t *config)
{
    assert(transport == &mock_ws && config->propagate_control_frames && !strcmp(config->ws_path, "/chat")); INJECT();
    assert(strstr(config->headers, "Authorization: Bearer secret-token\r\n") && strstr(config->headers, "Protocol-Version: 1\r\n"));
    return ESP_OK;
}
int esp_transport_connect(esp_transport_handle_t transport, const char *host, int port, int timeout)
{
    assert(transport == &mock_ws && !strcmp(host, "voice.example.org") && port == 443 && timeout == 5000);
    return injected(__func__) == ESP_OK ? 0 : -1;
}
int esp_transport_ws_send_raw(esp_transport_handle_t transport, ws_transport_opcodes_t opcode, const char *buffer, int size, int timeout)
{
    assert(transport == &mock_ws && (opcode & 128) && (timeout == 1000 || timeout == 20) && size >= 0);
    if (injected(__func__) != ESP_OK) return size ? size - 1 : -1;
    unsigned kind = opcode & 0x0f;
    if (kind == WS_TRANSPORT_OPCODES_BINARY) { assert(size == 640); ++audio_sends; }
    else if (kind == WS_TRANSPORT_OPCODES_PONG) { ++pongs; }
    else {
        assert(kind == WS_TRANSPORT_OPCODES_TEXT);
        if (strstr(buffer, "\"type\":\"listen\"")) {
            if (strstr(buffer, "\"state\":\"start\"")) ++listen_starts;
            else { assert(strstr(buffer, "\"state\":\"stop\"")); ++listen_stops; }
        } else if (strstr(buffer, "\"type\":\"abort\"")) ++aborts;
        else assert(strstr(buffer, "\"type\":\"hello\""));
    }
    /* 与 SDK 一样修改传入 RAM，检查调用者没有复用已 mask 的消息。 */
    for (int i = 0; i < size; ++i) ((char *)buffer)[i] ^= 0x55;
    return size;
}
static void frame_action(unsigned action)
{
    if (action == 1) finish();
    if (action == 2) cancel();
    if (action == 3) wifi_callback(callback_user, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, NULL);
    if (action == 4) clock_us += 31000000;
}
int esp_transport_poll_read(esp_transport_handle_t transport, int timeout)
{
    assert(transport == &mock_ws && (timeout == 0 || timeout == 20));
    if (injected(__func__) != ESP_OK) return -1;
    if (poll_empty && timeout == 0) { poll_empty = false; return 0; }
    if (frame_index == frame_count) { cancel(); return 0; }
    if (frames[frame_index].opcode == WS_TRANSPORT_OPCODES_NONE) {
        clock_us += 20000;
        frame_action(frames[frame_index++].action);
        return 0;
    }
    return 1;
}
int esp_transport_read(esp_transport_handle_t transport, char *buffer, int size, int timeout)
{
    assert(transport == &mock_ws && size == 4096 && timeout == 1000); INJECT();
    assert(frame_index < frame_count);
    clock_us += 20000;
    frame_t frame = frames[frame_index++];
    last_opcode = frame.opcode; last_fin = frame.fin; last_payload = frame.payload;
    frame_action(frame.action);
    if (frame.action == 5) {
        /* 模拟同一 WebSocket 帧两个 TCP 分段间隔 50 ms。 */
        if (timeout < 50) return -1;
        clock_us += 50000;
    }
    if (frame.size > 0) { assert(frame.size <= size); memcpy(buffer, frame.data, (size_t)frame.size); }
    return frame.size;
}
ws_transport_opcodes_t esp_transport_ws_get_read_opcode(esp_transport_handle_t transport) { assert(transport == &mock_ws); return (ws_transport_opcodes_t)last_opcode; }
int esp_transport_ws_get_read_payload_len(esp_transport_handle_t transport) { assert(transport == &mock_ws); return last_payload; }
bool esp_transport_ws_get_fin_flag(esp_transport_handle_t transport) { assert(transport == &mock_ws); return last_fin; }
int esp_transport_close(esp_transport_handle_t transport) { assert(transport == &mock_ws); ++ws_closes; return 0; }
esp_err_t esp_transport_destroy(esp_transport_handle_t transport)
{
    assert(transport->live);
    if (transport->ssl) { assert(!mock_ws.live); ++ssl_destroys; }
    else ++ws_destroys;
    transport->live = false; return ESP_OK;
}
esp_log_level_t esp_log_level_get(const char *tag) { assert(!strcmp(tag, "transport_ws")); return log_level; }
void esp_log_level_set(const char *tag, esp_log_level_t level) { assert(!strcmp(tag, "transport_ws")); log_level = level; }

esp_err_t bsp_audio_init(void) { INJECT(); if (!audio_initialized) audio_live = true; audio_initialized = true; return ESP_OK; }
esp_err_t bsp_audio_set_format(uint32_t rate, uint8_t bits, uint8_t channels) { assert(rate == 16000 && bits == 16 && channels == 1); INJECT(); return audio_live ? ESP_OK : ESP_ERR_INVALID_STATE; }
esp_err_t bsp_audio_wake(void) { INJECT(); audio_live = true; return ESP_OK; }
esp_err_t bsp_audio_sleep(void) { audio_live = false; ++audio_sleeps; INJECT(); return ESP_OK; }
void bsp_audio_set_volume(uint8_t volume) { assert(volume == 50); }
esp_err_t bsp_audio_read(void *data, size_t size) { assert(audio_live && size == 640); INJECT(); ++audio_reads; memset(data, 0x12, size); if (cancel_audio_read) cancel(); return ESP_OK; }
esp_err_t bsp_audio_write(const void *data, size_t size) { assert(audio_live && data && size == 640); INJECT(); ++audio_writes; return ESP_OK; }
esp_audio_err_t esp_opus_dec_open(void *raw, uint32_t size, void **handle)
{
    esp_opus_dec_cfg_t *config = raw;
    assert(size == sizeof(*config) && config->sample_rate == 16000 && config->channel == 1 && config->frame_duration == 8 && !config->self_delimited);
    INJECT(); *handle = &codec_live; codec_live = true; ++opus_opens; return ESP_AUDIO_ERR_OK;
}
esp_audio_err_t esp_opus_dec_reset(void *handle) { assert(handle == &codec_live && codec_live); INJECT(); ++opus_resets; return ESP_AUDIO_ERR_OK; }
esp_audio_err_t esp_opus_dec_decode(void *handle, esp_audio_dec_in_raw_t *raw, esp_audio_dec_out_frame_t *frame, esp_audio_dec_info_t *info)
{
    assert(handle == &codec_live && codec_live && raw->len > 0 && frame->len == 3840); INJECT();
    ++opus_decodes; raw->consumed = raw->len; frame->decoded_size = 640;
    info->sample_rate = 16000; info->channel = 1; info->bits_per_sample = 16;
    if (decode_fault == 1) --raw->consumed;
    if (decode_fault == 2) frame->decoded_size = 0;
    if (decode_fault == 3) frame->decoded_size = 3842;
    if (decode_fault == 4) frame->decoded_size = 639;
    if (decode_fault == 5) info->sample_rate = 24000;
    if (decode_fault == 6) info->channel = 2;
    if (decode_fault == 7) info->bits_per_sample = 8;
    if (cancel_decode) cancel();
    memset(frame->buffer, 0x14, 640);
    return ESP_AUDIO_ERR_OK;
}
esp_audio_err_t esp_opus_dec_close(void *handle) { assert(handle == &codec_live && codec_live); codec_live = false; ++opus_closes; return ESP_AUDIO_ERR_OK; }

esp_err_t linx_wifi_portal_start(linx_wifi_portal_t **portal, const linx_wifi_network_t *networks, size_t count)
{
    assert(ap_netif_live && wifi_mode == WIFI_MODE_APSTA && count <= 8 && networks); INJECT();
    assert(!mock_portal.live); mock_portal.live = true; *portal = &mock_portal; ++portal_starts;
    return ESP_OK;
}
bool linx_wifi_portal_poll(linx_wifi_portal_t *portal, linx_wifi_network_t *network, char password[65])
{
    assert(portal == &mock_portal && portal->live); ++portal_polls;
    if (portal_cancel) { cancel(); return false; }
    if (portal_replace) {
        portal_replace = false;
        linx_voice_request_t next = {.command = LINX_VOICE_START, .generation = 8};
        assert(linx_lingxi_device_request(&next, NULL));
        return false;
    }
    if (portal_time_out) { clock_us += SETUP_TIMEOUT_MS * 1000LL; return false; }
    if (portal_submissions == 0) return false;
    --portal_submissions; ++portal_submission_count;
    if (drop_retry_disconnect && portal_submission_count == 2) drop_disconnect = true;
    strcpy(network->ssid, "phone-network"); network->security = LINX_WIFI_PERSONAL;
    strcpy(password, "phone-secret");
    wifi_auto_connect = portal_failures == 0;
    if (portal_failures) --portal_failures;
    return true;
}
void linx_wifi_portal_status(linx_wifi_portal_t *portal, linx_wifi_portal_state_t state)
{
    assert(portal == &mock_portal && portal->live); ++portal_statuses[state];
    if (state == LINX_WIFI_PORTAL_SAVED && portal_cancel_success) cancel();
}
esp_err_t linx_wifi_portal_stop(linx_wifi_portal_t *portal) { assert(portal == &mock_portal && portal->live); ++portal_stops; if (portal_stop_failures) { --portal_stop_failures; return ESP_FAIL; } INJECT(); portal->live = false; return ESP_OK; }

static void reset(void)
{
    assert(!wifi_live && !wifi_running && !loop_live && !netif_live && !codec_live && !audio_live && !ap_netif_live && !mock_portal.live);
    assert(!mock_http.live && !mock_ssl.live && !mock_ws.live && !sntp_running && !pending_cleanup);
    if (commands) vQueueDelete(commands);
    if (events) vQueueDelete(events);
    commands = NULL; events = NULL;
    atomic_store(&active, false); atomic_store(&active_generation, 0);
    atomic_store(&listening, false); atomic_store(&finish_pending, false);
    audio_initialized = false;
    failure = NULL; failure_code = ESP_FAIL; failure_index = 1; failure_calls = 0;
    session_oom = existing_loop = existing_netif = false; wifi_auto_connect = true;
    nvs_missing = true; nvs_invalid = false; configured = true; sntp_existing = false;
    cancel_delay = cancel_perform = cancel_decode = cancel_audio_read = false;
    disconnect_delay = time_ready_delay = 0; delay_calls = 0;
    clock_us = 0; wall_time = 1800000000;
    strcpy(persisted_uuid, "12345678-1234-4123-8123-123456789012");
    memset(event_counts, 0, sizeof(event_counts));
    queue_calls = task_calls = session_allocations = 0;
    wifi_stops = wifi_deinits = netif_destroys = loop_deletes = unregisters = nvs_closes = 0;
    http_cleanups = ssl_destroys = ws_destroys = ws_closes = sntp_stops = 0;
    sntp_starts = 0; sntp_response_server = -1; sntp_started_us = 0;
    memset(sntp_servers, 0, sizeof(sntp_servers));
    audio_reads = audio_writes = audio_sleeps = opus_opens = opus_resets = opus_decodes = opus_closes = 0;
    listen_starts = listen_stops = aborts = audio_sends = pongs = 0;
    decode_fault = 0; http_status = 200;
    http_body = "{\"websocket\":{\"url\":\"wss://voice.example.org/chat\",\"token\":\"secret-token\"}}";
    http_negative = http_overflow = poll_empty = false;
    log_level = 3; handler_count = 0;
    frame_count = frame_index = 0;
    last_opcode = WS_TRANSPORT_OPCODES_NONE; last_fin = true; last_payload = 0;
    test_ssid = ""; test_password = "";
    existing_ap_netif = false; wifi_mode = WIFI_MODE_STA;
    disconnect_pending_event = sync_pending_event = drop_disconnect = drop_barrier = drop_retry_disconnect = false;
    barrier_posts = barriers_dispatched = delayed_disconnects = 0; sync_callback = NULL;
    ap_sets = ap_destroys = disconnects = connect_calls = 0;
    portal_stop_failures = portal_starts = portal_stops = portal_polls = portal_submissions = portal_failures = portal_submission_count = cancel_delay_at = 0;
    portal_replace = disconnect_returns_not_connected = false;
    memset(portal_statuses, 0, sizeof(portal_statuses));
    portal_cancel = portal_disconnect_error = portal_time_out = portal_cancel_success = false;
    ignore_association = unrelated_association = old_ip_after_disconnect = false;
    memset(&flash_config, 0, sizeof(flash_config));
    strcpy((char *)flash_config.sta.ssid, "existing-net");
    strcpy((char *)flash_config.sta.password, "old-secret");
    ram_config = flash_config; wifi_storage = WIFI_STORAGE_FLASH;
    ram_sets = flash_sets = config_noops = cancel_ram_at = 0;
    cancel_on_disconnect = flash_error_after_ram = false;
    cancel_on_flash = cancel_on_ram = cancel_on_connect = cancel_on_radio = false;
    scan_auto_complete = true; scan_live = scan_invalid_count = cancel_scan_results = replace_scan_with_save = false;
    scan_status = scan_clears = scan_stops = scan_calls = 0;
    memset(scan_records, 0, sizeof(scan_records)); scan_record_count = 0;
}

static void add_text(const char *text)
{
    frames[frame_count++] = (frame_t){.opcode = 1, .fin = true, .payload = (int)strlen(text), .data = text, .size = (int)strlen(text)};
}
static void add_binary(const void *data, int size)
{
    frames[frame_count++] = (frame_t){.opcode = 2, .fin = true, .payload = size, .data = data, .size = size};
}
static void add_wait(unsigned action)
{
    frames[frame_count++] = (frame_t){.opcode = 256, .fin = true, .action = action};
}
static void begin(void)
{
    assert(linx_lingxi_device_init());
    assert(queue_calls == 2 && task_calls == 1 && session_allocations == 0);
    assert(!wifi_live && !audio_live);
    linx_voice_request_t request = {.command = LINX_VOICE_START, .generation = 7};
    assert(linx_lingxi_device_request(&request, NULL));
}
static void execute(void)
{
    if (!setjmp(worker_end)) worker_function(NULL);
    assert(log_level == 3);
    assert(!mock_http.live && !mock_ssl.live && !mock_ws.live);
    assert(!wifi_live && !wifi_running && !netif_live && !loop_live && !audio_live && !codec_live);
}
static void expect_error_at(linx_voice_error_t error, int line)
{
    linx_voice_event_t event;
    assert(linx_lingxi_device_poll(&event));
    if (event.state != LINX_VOICE_ERROR || event.error != error) {
        fprintf(stderr, "line %d: expected error %d, actual state %d error %d (injection %s)\n", line, error, event.state, event.error, failure ? failure : "none");
        assert(false);
    }
    assert(event.generation == 7);
    assert(!linx_lingxi_device_poll(&event));
}

#define expect_error(error) expect_error_at(error, __LINE__)

static void test_init_and_commands(void)
{
    reset();
    assert(!linx_lingxi_device_poll(NULL));
    linx_voice_event_t event;
    linx_voice_request_t request = {.command = LINX_VOICE_START, .generation = 7};
    assert(!linx_lingxi_device_poll(&event));
    request.command = LINX_VOICE_FINISH;
    assert(!linx_lingxi_device_request(&request, NULL));
    request.command = LINX_VOICE_START;
    failure = "xQueueCreate"; failure_index = 1;
    assert(!linx_lingxi_device_init() && !commands && !events);
    failure_calls = 0; failure_index = 2;
    assert(!linx_lingxi_device_init() && !commands && !events);
    failure = "xTaskCreate"; failure_calls = 0; failure_index = 1;
    assert(!linx_lingxi_device_init() && !commands && !events);
    failure_calls = 0;
    assert(!linx_lingxi_device_request(&request, NULL) && !commands && !events);
    failure = NULL;
    assert(linx_lingxi_device_request(&request, NULL) && commands && events);
    cancel(); execute();
    reset(); begin(); assert(linx_lingxi_device_init() && task_calls == 1);
    assert(!linx_lingxi_device_request(NULL, NULL));
    request.command = (linx_voice_command_t)-1;
    assert(!linx_lingxi_device_request(&request, NULL));
    request.command = (linx_voice_command_t)99;
    assert(!linx_lingxi_device_request(&request, NULL));
    request.command = LINX_VOICE_FINISH; request.generation = 3;
    assert(linx_lingxi_device_request(&request, NULL) && !atomic_load(&finish_pending));
    request.generation = 7;
    assert(linx_lingxi_device_request(&request, NULL) && !atomic_load(&finish_pending));
    cancel(); execute();
    assert(session_allocations == 0 && linx_lingxi_device_poll(&event) && event.state == LINX_VOICE_CLOSED);
    reset(); begin(); session_oom = true; execute(); expect_error(LINX_VOICE_NO_MEMORY);
}

static void test_success_stream(void)
{
    reset(); begin();
    add_text(hello_opus);
    add_wait(1);
    add_text("{\"type\":\"stt\",\"text\":\"hello\",\"session_id\":\"session-1\"}");
    add_text(tts_start);
    static const uint8_t packet[] = {1, 2, 3, 4};
    frames[frame_count++] = (frame_t){.opcode = 2, .fin = true, .payload = 4, .data = packet, .size = 1};
    frames[frame_count++] = (frame_t){.opcode = 2, .fin = true, .payload = 4, .data = packet + 1, .size = 3};
    add_text(tts_stop);
    add_wait(2);
    execute();
    assert(listen_starts == 2 && listen_stops == 1 && aborts == 1);
    assert(audio_sends == 2 && audio_reads == 7 && audio_writes == 1);
    assert(opus_opens == 1 && opus_resets == 1 && opus_decodes == 1 && opus_closes == 1 && audio_sleeps == 1);
    assert(http_cleanups == 1 && ws_closes == 1 && ws_destroys == 1 && ssl_destroys == 1);
    assert(wifi_stops == 1 && wifi_deinits == 1 && netif_destroys == 1 && unregisters == 2 && loop_deletes == 1);
    assert(event_counts[LINX_VOICE_LISTENING] == 2 && event_counts[LINX_VOICE_WAITING] == 1 && event_counts[LINX_VOICE_SPEAKING] == 1);
    assert(persisted_uuid[14] == '4');
    linx_voice_event_t event; assert(linx_lingxi_device_poll(&event) && event.state == LINX_VOICE_CLOSED);
}

static void test_wifi_and_identity_failures(void)
{
    const char *invalid_uuid[] = {"short", "12345678-1234-1123-8123-123456789012", "12345678-1234-4123-7123-123456789012", "12345678x1234-4123-8123-123456789012", "z2345678-1234-4123-8123-123456789012"};
    for (unsigned i = 0; i < sizeof(invalid_uuid) / sizeof(*invalid_uuid); ++i) {
        reset(); begin(); nvs_missing = false; strcpy(persisted_uuid, invalid_uuid[i]); execute(); expect_error(LINX_VOICE_WIFI_FAILED);
    }
    const char *operations[] = {"nvs_flash_init", "esp_read_mac", "nvs_open", "nvs_get_str", "nvs_set_str", "nvs_commit", "esp_netif_init", "esp_event_loop_create_default", "esp_netif_attach_wifi_station", "esp_wifi_set_default_wifi_sta_handlers", "esp_wifi_init", "esp_event_handler_instance_register", "esp_wifi_set_storage", "esp_wifi_set_mode", "esp_wifi_get_config", "esp_wifi_set_config", "esp_wifi_start", "esp_wifi_connect", "esp_wifi_sta_get_ap_info"};
    for (unsigned i = 0; i < sizeof(operations) / sizeof(*operations); ++i) {
        for (unsigned oom = 0; oom < 2; ++oom) {
            reset(); begin(); test_ssid = "new-net"; test_password = "password";
            failure = operations[i]; failure_code = oom ? ESP_ERR_NO_MEM : ESP_FAIL;
            if (!strcmp(failure, "esp_wifi_set_config")) configured = false;
            execute(); expect_error(oom ? LINX_VOICE_NO_MEMORY : LINX_VOICE_WIFI_FAILED);
            if (i >= 3) assert(nvs_closes == 1);
        }
    }
    reset(); begin(); failure = "esp_event_handler_instance_register"; failure_index = 2; execute(); expect_error(LINX_VOICE_WIFI_FAILED); assert(unregisters == 1);
    reset(); begin(); failure = "esp_netif_new"; execute(); expect_error(LINX_VOICE_NO_MEMORY);
    reset(); begin(); existing_netif = true; existing_loop = true; execute(); expect_error(LINX_VOICE_WIFI_FAILED); assert(netif_destroys == 0 && loop_deletes == 0);
    reset(); begin(); configured = false; execute(); expect_error(LINX_VOICE_NOT_CONFIGURED);
    reset(); begin(); configured = false; test_ssid = "123456789012345678901234567890123"; execute(); expect_error(LINX_VOICE_NOT_CONFIGURED);
    reset(); begin(); configured = false; test_ssid = "net"; test_password = "12345678901234567890123456789012345678901234567890123456789012345"; execute(); expect_error(LINX_VOICE_NOT_CONFIGURED);
    reset(); begin(); nvs_missing = false; nvs_invalid = true; execute(); expect_error(LINX_VOICE_WIFI_FAILED);
    reset(); begin(); nvs_missing = false; existing_loop = true; execute(); assert(loop_deletes == 0 && http_cleanups == 1);
    reset(); begin(); wifi_auto_connect = false; execute(); expect_error(LINX_VOICE_TIMEOUT); assert(delay_calls == 500);
    reset(); begin(); wifi_auto_connect = false; disconnect_delay = 1; execute(); expect_error(LINX_VOICE_WIFI_FAILED);
    reset(); begin(); wifi_auto_connect = false; cancel_delay = true; execute(); assert(http_cleanups == 0 && wifi_stops == 1);
}

static void test_cold_boot_time(void)
{
    /* 首包未及时应答时，SDK 在 15 秒之后才会重试或请求备用服务器。 */
    reset(); begin(); wall_time = 0; time_ready_delay = 20000 / READ_TIMEOUT_MS;
    http_body = "{\"activation\":{\"code\":\"123456\"}}";
    execute();
    assert(wall_time >= 1704067200 && http_cleanups == 1 && sntp_stops == 1);
    assert(event_counts[LINX_VOICE_ERROR] == 0);

    const char *servers[] = {"ntp.aliyun.com", "ntp.tencent.com", "pool.ntp.org"};
    for (int server = 0; server < 3; ++server) {
        reset(); begin(); wall_time = 0; sntp_response_server = server;
        http_body = "{\"activation\":{\"code\":\"123456\"}}";
        execute();
        assert(wall_time >= 1704067200 && sntp_starts == 1 && sntp_stops == 1);
        assert(http_cleanups == 1 && event_counts[LINX_VOICE_ERROR] == 0);
        assert(clock_us >= (5000 + 15000 * server) * 1000LL && clock_us < 60000000);
        for (unsigned i = 0; i < 3; ++i) assert(sntp_servers[i] && !strcmp(sntp_servers[i], servers[i]));
    }

    reset(); begin(); wall_time = 0; execute(); expect_error(LINX_VOICE_TIME_FAILED);
    assert(clock_us == 60000000 && http_cleanups == 0 && sntp_starts == 1 && sntp_stops == 1);
    reset(); begin(); wall_time = 0; cancel_delay_at = 800; execute();
    assert(clock_us == 16000000 && http_cleanups == 0 && sntp_stops == 1 && event_counts[LINX_VOICE_ERROR] == 0);
    reset(); begin(); wall_time = 0; disconnect_delay = 800; execute(); expect_error(LINX_VOICE_WIFI_FAILED);
    assert(clock_us == 16000000 && http_cleanups == 0 && sntp_stops == 1);
    reset(); begin(); wall_time = 1704067200; http_body = "{\"activation\":{\"code\":\"123456\"}}";
    execute(); assert(sntp_starts == 0 && sntp_stops == 0 && http_cleanups == 1 && delay_calls == 0);
}

static void test_time_and_gateway(void)
{
    const char *cleanup_operations[] = {"esp_wifi_stop", "esp_wifi_deinit", "esp_event_loop_delete_default"};
    for (unsigned i = 0; i < sizeof(cleanup_operations) / sizeof(*cleanup_operations); ++i) {
        reset(); begin(); http_body = "{\"activation\":{\"code\":\"123456\"}}";
        failure = cleanup_operations[i]; execute(); expect_error(LINX_VOICE_WIFI_FAILED);
    }
    reset(); begin(); wall_time = 0; execute(); expect_error(LINX_VOICE_TIME_FAILED); assert(sntp_stops == 1);
    reset(); begin(); wall_time = 0; time_ready_delay = 1; execute(); assert(http_cleanups == 1 && sntp_stops == 1);
    reset(); begin(); wall_time = 0; sntp_existing = true; time_ready_delay = 1; execute(); assert(sntp_stops == 0);
    reset(); begin(); wall_time = 0; disconnect_delay = 1; execute(); expect_error(LINX_VOICE_WIFI_FAILED);
    reset(); begin(); wall_time = 0; cancel_delay = true; execute(); assert(http_cleanups == 0 && sntp_stops == 1);
    const char *operations[] = {"esp_http_client_set_method", "esp_http_client_set_header", "esp_http_client_set_post_field", "esp_http_client_perform"};
    for (unsigned i = 0; i < sizeof(operations) / sizeof(*operations); ++i) {
        reset(); begin(); failure = operations[i]; execute(); expect_error(LINX_VOICE_NETWORK_FAILED); assert(http_cleanups == 1);
    }
    for (int i = 2; i <= 5; ++i) { reset(); begin(); failure = "esp_http_client_set_header"; failure_index = i; failure_code = ESP_ERR_NO_MEM; execute(); expect_error(LINX_VOICE_NO_MEMORY); }
    reset(); begin(); failure = "esp_http_client_init"; execute(); expect_error(LINX_VOICE_NO_MEMORY);
    reset(); begin(); http_status = 500; execute(); expect_error(LINX_VOICE_NETWORK_FAILED);
    reset(); begin(); http_body = "{\"error\":\"not authorized\"}"; execute(); expect_error(LINX_VOICE_PROTOCOL_FAILED);
    reset(); begin(); http_negative = true; execute(); expect_error(LINX_VOICE_PROTOCOL_FAILED);
    reset(); begin(); http_overflow = true; execute(); expect_error(LINX_VOICE_PROTOCOL_FAILED);
    reset(); begin(); cancel_perform = true; execute(); assert(ssl_destroys == 0 && http_cleanups == 1 && event_counts[LINX_VOICE_ERROR] == 0);
    reset(); begin(); http_body = "{\"activation\":{\"code\":\"123456\"}}"; execute();
    linx_voice_event_t event; assert(linx_lingxi_device_poll(&event) && event.state == LINX_VOICE_BINDING && !strcmp(event.activation_code, "123456"));
    assert(ssl_destroys == 0 && audio_sleeps == 0 && http_cleanups == 1);
}

static void test_transport_and_audio_errors(void)
{
    const char *operations[] = {"esp_transport_ssl_init", "esp_transport_ws_init", "esp_transport_ws_set_config", "esp_transport_connect", "esp_transport_ws_send_raw", "esp_transport_read", "bsp_audio_init", "bsp_audio_set_format", "bsp_audio_wake", "esp_opus_dec_open", "esp_opus_dec_reset", "esp_opus_dec_decode", "bsp_audio_write", "bsp_audio_read"};
    for (unsigned i = 0; i < sizeof(operations) / sizeof(*operations); ++i) {
        reset(); begin(); add_text(hello_opus); add_wait(1); add_text(tts_start); add_binary("opus", 4);
        failure = operations[i];
        if (i == 0 || i == 1) { execute(); expect_error(LINX_VOICE_NO_MEMORY); }
        else if (i <= 5) { execute(); expect_error(LINX_VOICE_NETWORK_FAILED); }
        else { execute(); expect_error(LINX_VOICE_AUDIO_FAILED); }
    }
    reset(); begin(); add_text(hello_opus); failure = "esp_transport_poll_read"; execute(); expect_error(LINX_VOICE_NETWORK_FAILED);
    reset(); begin(); failure = "esp_transport_ws_set_config"; failure_code = ESP_ERR_NO_MEM; execute(); expect_error(LINX_VOICE_NO_MEMORY);
    reset(); begin(); add_text(hello_opus); failure = "esp_opus_dec_open"; failure_code = ESP_AUDIO_ERR_MEM_LACK; execute(); expect_error(LINX_VOICE_NO_MEMORY);
    reset(); begin(); add_text(hello_opus); add_text(tts_start); add_binary("opus", 4); failure = "esp_opus_dec_decode"; failure_code = ESP_AUDIO_ERR_MEM_LACK; execute(); expect_error(LINX_VOICE_NO_MEMORY);
    for (unsigned fault = 1; fault <= 7; ++fault) { reset(); begin(); add_text(hello_opus); add_text(tts_start); add_binary("opus", 4); decode_fault = fault; execute(); expect_error(LINX_VOICE_AUDIO_FAILED); assert(audio_writes == 0); }
    reset(); begin(); add_text(hello_opus); add_text(tts_start); add_binary("opus", 4); cancel_decode = true; execute(); assert(opus_decodes == 1 && audio_writes == 0);
    reset(); begin(); voice_session_t *session = test_session_alloc(1, sizeof(*session)); session->audio_awake = true; failure = "bsp_audio_sleep"; assert(cleanup(session) == LINX_VOICE_AUDIO_FAILED); cancel(); execute();
    reset(); begin(); add_text(hello_opus); cancel_audio_read = true; execute(); assert(audio_reads == 1 && audio_sends == 0);
    reset(); begin(); add_text(hello_pcm); add_text(tts_start); static uint8_t pcm[640]; add_binary(pcm, sizeof(pcm)); add_text(tts_stop); execute(); assert(opus_opens == 0 && audio_writes == 1 && listen_starts == 2);
    reset(); begin(); add_text(hello_pcm); add_text(tts_start); add_binary("odd", 3); execute(); expect_error(LINX_VOICE_PROTOCOL_FAILED);
    reset(); begin(); add_text(hello_pcm); add_text(tts_start); static uint8_t excessive[3842]; add_binary(excessive, sizeof(excessive)); execute(); expect_error(LINX_VOICE_PROTOCOL_FAILED);
}

static void test_protocol_sequences_and_cancel(void)
{
    reset(); begin(); add_text("not JSON"); execute(); expect_error(LINX_VOICE_PROTOCOL_FAILED);
    reset(); begin(); add_text(tts_start); execute(); expect_error(LINX_VOICE_PROTOCOL_FAILED);
    reset(); begin(); add_binary("opus", 4); execute(); expect_error(LINX_VOICE_PROTOCOL_FAILED);
    reset(); begin(); add_text(hello_opus); add_text(hello_opus); execute(); expect_error(LINX_VOICE_PROTOCOL_FAILED);
    reset(); begin(); add_text(hello_opus); add_text("{\"type\":\"tts\",\"state\":\"start\",\"session_id\":\"other\"}"); execute(); expect_error(LINX_VOICE_PROTOCOL_FAILED);
    reset(); begin(); add_text(hello_opus); add_text(tts_stop); execute(); expect_error(LINX_VOICE_PROTOCOL_FAILED);
    reset(); begin(); add_text(hello_opus); add_text(tts_start); add_text("{\"type\":\"tts\",\"state\":\"stop\",\"is_aborted\":true}"); execute(); assert(listen_starts == 2 && audio_sleeps == 2 && event_counts[LINX_VOICE_ERROR] == 0);
    reset(); begin(); frames[frame_count++] = (frame_t){.opcode = 8, .fin = true}; execute(); expect_error(LINX_VOICE_NETWORK_FAILED);
    reset(); begin(); frames[frame_count++] = (frame_t){.opcode = 1, .fin = true, .payload = -1}; execute(); expect_error(LINX_VOICE_PROTOCOL_FAILED);
    reset(); begin(); frames[frame_count++] = (frame_t){.opcode = 9, .fin = true}; frames[frame_count++] = (frame_t){.opcode = 10, .fin = true}; add_text(hello_opus); execute(); assert(pongs == 1 && listen_starts == 1);
    reset(); begin(); frames[frame_count++] = (frame_t){.opcode = 1, .fin = true, .payload = 4097, .size = 1, .data = "x"}; execute(); expect_error(LINX_VOICE_PROTOCOL_FAILED);
    reset(); begin(); add_wait(4); execute(); expect_error(LINX_VOICE_TIMEOUT);
    reset(); begin(); add_text(hello_opus); add_wait(1); add_wait(4); execute(); expect_error(LINX_VOICE_TIMEOUT);
    reset(); begin(); add_text(hello_opus); add_text(tts_start); add_wait(4); execute(); expect_error(LINX_VOICE_TIMEOUT);
    reset(); begin(); add_text(hello_opus); add_wait(3); execute(); expect_error(LINX_VOICE_WIFI_FAILED);
    reset(); begin(); add_text(hello_opus); poll_empty = true; execute(); assert(audio_reads == 2 && audio_sends == 2);
    reset(); begin(); frames[frame_count++] = (frame_t){.opcode = 9, .fin = false}; execute(); expect_error(LINX_VOICE_PROTOCOL_FAILED);
    reset(); begin(); frames[frame_count++] = (frame_t){.opcode = 9, .fin = true, .payload = 126}; execute(); expect_error(LINX_VOICE_PROTOCOL_FAILED);
    reset(); begin(); frames[frame_count++] = (frame_t){.opcode = 9, .fin = true, .payload = 1}; execute(); expect_error(LINX_VOICE_NETWORK_FAILED);
    reset(); begin(); add_text(hello_opus); add_wait(1); add_wait(1); add_text(tts_start); add_text(tts_stop); execute(); assert(listen_starts == 2 && listen_stops == 1);
    reset(); begin(); linx_voice_event_t stale = {.generation = 2, .state = LINX_VOICE_ERROR}; xQueueOverwrite(events, &stale);
    linx_voice_event_t event; assert(!linx_lingxi_device_poll(&event)); cancel();
    stale.generation = 7; xQueueOverwrite(events, &stale); assert(!linx_lingxi_device_poll(&event)); execute();
    reset(); begin(); linx_voice_request_t replacement = {.command = LINX_VOICE_START, .generation = 8};
    assert(linx_lingxi_device_request(&replacement, NULL));
    linx_voice_request_t late = {.command = LINX_VOICE_CANCEL, .generation = 7};
    assert(linx_lingxi_device_request(&late, NULL) && current(8));
    execute(); assert(session_allocations == 1 && atomic_load(&active_generation) == 8);
}

static void test_xiaozhi_minimal_hello(void)
{
    reset(); begin();
    http_body = "{\"websocket\":{\"url\":\"wss://voice.example.org/chat\",\"token\":\"Bearer secret-token\"}}";
    add_text(hello_opus); execute();
    assert(listen_starts == 1 && event_counts[LINX_VOICE_ERROR] == 0);
    reset(); begin();
    add_text("{\"type\":\"hello\",\"transport\":\"websocket\",\"audio_params\":{\"sample_rate\":24000}}");
    add_text("{\"type\":\"tts\",\"state\":\"start\"}");
    add_binary("opus", 4);
    add_text("{\"type\":\"tts\",\"state\":\"stop\"}");
    execute();
    assert(listen_starts == 2 && opus_opens == 1 && opus_decodes == 1 && audio_writes == 1);
    assert(event_counts[LINX_VOICE_ERROR] == 0);
}

static void test_frame_io_timeout(void)
{
    reset(); begin(); add_text(hello_opus); frames[0].action = 5;
    add_text(tts_start);
    frames[frame_count++] = (frame_t){.opcode = 2, .fin = true, .payload = 4, .data = "o", .size = 1};
    frames[frame_count++] = (frame_t){.opcode = 2, .fin = true, .payload = 4, .data = "pus", .size = 3, .action = 5};
    add_text(tts_stop); execute();
    assert(opus_decodes == 1 && audio_writes == 1 && event_counts[LINX_VOICE_ERROR] == 0);
    reset(); begin(); add_text(hello_opus);
    frames[frame_count++] = (frame_t){.opcode = 2, .fin = true, .payload = 4};
    execute(); expect_error(LINX_VOICE_NETWORK_FAILED);
}

static void test_playback_drain_and_control(void)
{
    reset(); begin();
    static uint8_t ping[125];
    frames[frame_count++] = (frame_t){.opcode = 9, .fin = true, .payload = 125, .data = ping, .size = 40};
    frames[frame_count++] = (frame_t){.opcode = 9, .fin = true, .payload = 125, .data = ping + 40, .size = 85};
    add_text(hello_opus); execute(); assert(pongs == 1 && listen_starts == 1);
    reset(); begin(); add_text(hello_opus); add_text(tts_start); add_text(tts_stop);
    cancel_delay = true; execute(); assert(listen_starts == 1 && delay_calls == 1);
    reset(); begin(); add_text(hello_opus); add_text(tts_start); add_text(tts_stop);
    failure = "bsp_audio_read"; execute(); expect_error(LINX_VOICE_AUDIO_FAILED);
    reset(); begin(); add_text(hello_opus); add_text(tts_start); add_text(tts_stop);
    cancel_audio_read = true; execute(); assert(listen_starts == 1 && audio_sends == 0);
    reset(); begin(); add_text(hello_opus); add_text(tts_start);
    add_text("{\"type\":\"tts\",\"state\":\"stop\",\"is_aborted\":true}");
    failure = "bsp_audio_wake"; failure_index = 2;
    execute(); expect_error(LINX_VOICE_AUDIO_FAILED);
    reset(); begin(); add_text(hello_opus); add_text(tts_start);
    add_text("{\"type\":\"tts\",\"state\":\"stop\",\"is_aborted\":true}");
    failure = "bsp_audio_sleep";
    execute(); expect_error(LINX_VOICE_AUDIO_FAILED);
}

static void test_audio_resume(void)
{
    reset(); begin(); add_text(hello_opus); add_wait(2); execute();
    assert(audio_initialized && !audio_live && audio_sleeps == 1);
    assert(bsp_audio_set_format(16000, 16, 1) == ESP_ERR_INVALID_STATE);
    frame_count = frame_index = 0;
    linx_voice_request_t request = {.command = LINX_VOICE_START, .generation = 8};
    assert(linx_lingxi_device_request(&request, NULL));
    add_text(hello_opus); add_wait(2); execute();
    assert(audio_sleeps == 2 && opus_opens == 2 && event_counts[LINX_VOICE_ERROR] == 0);
}

static void begin_wifi(linx_voice_command_t command)
{
    begin();
    linx_voice_request_t request = {.command = command, .generation = 7};
    if (command == LINX_VOICE_WIFI_SAVE) {
        strcpy(request.wifi_network.ssid, "new-network");
        request.wifi_network.security = LINX_WIFI_PERSONAL;
        strcpy(request.wifi_password, "new-secret");
    }
    assert(linx_lingxi_device_request(&request, NULL));
    memset(&request, 0, sizeof(request));
}

static void assert_old_network(void)
{
    assert(!strcmp((char *)flash_config.sta.ssid, "existing-net"));
    assert(!strcmp((char *)flash_config.sta.password, "old-secret"));
}

static void test_wifi_scan(void)
{
    reset(); begin_wifi(LINX_VOICE_WIFI_SCAN);
    /* 包含隐藏网络、同名不同认证、企业网络及超过显示容量的 AP。 */
    scan_record_count = SCAN_RECORD_CAPACITY;
    for (unsigned i = 0; i < scan_record_count; ++i) {
        snprintf((char *)scan_records[i].ssid, sizeof(scan_records[i].ssid), "network-%02u", i);
        scan_records[i].rssi = (int8_t)(-80 + (int)i * 3);
        scan_records[i].authmode = WIFI_AUTH_WPA2_PSK;
    }
    scan_records[15].ssid[0] = 0;
    strcpy((char *)scan_records[14].ssid, "shared"); scan_records[14].authmode = WIFI_AUTH_OPEN;
    strcpy((char *)scan_records[13].ssid, "shared"); scan_records[13].authmode = WIFI_AUTH_WPA2_PSK;
    strcpy((char *)scan_records[12].ssid, "enterprise"); scan_records[12].authmode = WIFI_AUTH_WPA2_ENTERPRISE;
    strcpy((char *)scan_records[11].ssid, "personal"); scan_records[11].authmode = WIFI_AUTH_WPA3_PSK;
    strcpy((char *)scan_records[10].ssid, "personal"); scan_records[10].authmode = WIFI_AUTH_WPA2_WPA3_PSK;
    scan_records[9].authmode = WIFI_AUTH_WPA_PSK;
    scan_records[8].authmode = WIFI_AUTH_WPA_WPA2_PSK;
    scan_records[7].authmode = WIFI_AUTH_OPEN;
    execute();
    linx_voice_event_t event; assert(linx_lingxi_device_poll(&event));
    assert(event.state == LINX_VOICE_WIFI_NETWORKS && event.wifi_count == 8);
    assert(!strcmp(event.wifi_networks[0].ssid, "shared") && event.wifi_networks[0].security == LINX_WIFI_UNSUPPORTED);
    assert(!strcmp(event.wifi_networks[1].ssid, "enterprise") && event.wifi_networks[1].security == LINX_WIFI_UNSUPPORTED);
    assert(event.wifi_networks[2].security == LINX_WIFI_PERSONAL && event.wifi_networks[5].security == LINX_WIFI_OPEN);
    assert(!strcmp(event.wifi_networks[3].ssid, "network-09") && event.wifi_networks[3].security == LINX_WIFI_UNSUPPORTED);
    assert(!strcmp(event.wifi_networks[4].ssid, "network-08") && event.wifi_networks[4].security == LINX_WIFI_PERSONAL);
    for (size_t i = 1; i < event.wifi_count; ++i) assert(event.wifi_networks[i - 1].rssi >= event.wifi_networks[i].rssi);
    assert(scan_calls == 1 && scan_stops == 1 && scan_clears == 1);
    assert(event_counts[LINX_VOICE_WIFI_SCANNING] == 1 && http_cleanups == 0 && nvs_closes == 0 && audio_reads == 0);
    assert_old_network();

    reset(); begin_wifi(LINX_VOICE_WIFI_SCAN); execute();
    assert(linx_lingxi_device_poll(&event) && event.state == LINX_VOICE_WIFI_NETWORKS && event.wifi_count == 0);
    const char *operations[] = {"nvs_flash_init", "esp_wifi_start", "esp_wifi_scan_start", "esp_wifi_scan_get_ap_records", "esp_wifi_clear_ap_list"};
    for (unsigned i = 0; i < sizeof(operations) / sizeof(*operations); ++i) {
        reset(); begin_wifi(LINX_VOICE_WIFI_SCAN); failure = operations[i]; execute(); expect_error(LINX_VOICE_WIFI_FAILED); assert_old_network();
    }
    reset(); begin_wifi(LINX_VOICE_WIFI_SCAN); failure = "esp_wifi_scan_start"; failure_code = ESP_ERR_NO_MEM; execute(); expect_error(LINX_VOICE_NO_MEMORY);
    reset(); begin_wifi(LINX_VOICE_WIFI_SCAN); scan_status = 1; execute(); expect_error(LINX_VOICE_WIFI_FAILED); assert(scan_stops == 1 && scan_clears == 1);
    reset(); begin_wifi(LINX_VOICE_WIFI_SCAN); scan_invalid_count = true; execute(); expect_error(LINX_VOICE_WIFI_FAILED);
    reset(); begin_wifi(LINX_VOICE_WIFI_SCAN); scan_auto_complete = false; execute(); expect_error(LINX_VOICE_TIMEOUT); assert(delay_calls == 500 && scan_stops == 1 && scan_clears == 1);
    reset(); begin_wifi(LINX_VOICE_WIFI_SCAN); scan_auto_complete = false; cancel_delay = true; execute();
    assert(scan_stops == 1 && scan_clears == 1 && event_counts[LINX_VOICE_WIFI_NETWORKS] == 0 && event_counts[LINX_VOICE_ERROR] == 0);
    reset(); begin_wifi(LINX_VOICE_WIFI_SCAN); cancel_on_radio = true; execute(); assert(scan_calls == 0 && flash_sets == 0);
    reset(); begin_wifi(LINX_VOICE_WIFI_SCAN); cancel_scan_results = true; execute(); assert(event_counts[LINX_VOICE_WIFI_NETWORKS] == 0 && event_counts[LINX_VOICE_ERROR] == 0);
    reset(); begin_wifi(LINX_VOICE_WIFI_SCAN); cancel(); execute(); assert(session_allocations == 0 && scan_calls == 0);
    reset(); begin_wifi(LINX_VOICE_WIFI_SCAN); scan_auto_complete = false; replace_scan_with_save = true; execute();
    assert(session_allocations == 2 && task_calls == 1 && scan_stops == 1 && scan_clears == 1);
    assert(event_counts[LINX_VOICE_WIFI_NETWORKS] == 0 && event_counts[LINX_VOICE_WIFI_SAVED] == 1);
    assert(linx_lingxi_device_poll(&event) && event.generation == 8 && event.state == LINX_VOICE_WIFI_SAVED);
    assert(!strcmp((char *)flash_config.sta.ssid, "next-network"));
}

static void test_wifi_save(void)
{
    reset(); begin_wifi(LINX_VOICE_WIFI_SAVE); execute();
    linx_voice_event_t event; assert(linx_lingxi_device_poll(&event));
    assert(event.state == LINX_VOICE_WIFI_SAVED && event_counts[LINX_VOICE_WIFI_CONNECTING] == 1);
    assert(!strcmp((char *)flash_config.sta.ssid, "new-network") && !strcmp((char *)flash_config.sta.password, "new-secret"));
    assert(ram_sets == 2 && flash_sets == 1 && http_cleanups == 0 && nvs_closes == 0);
    assert(!wifi_live && audio_reads == 0 && audio_writes == 0);
    /* NVS 网络优先于编译时设置。 */
    test_ssid = "compile-fallback"; test_password = "fallback-secret";
    linx_voice_request_t voice = {.command = LINX_VOICE_START, .generation = 8};
    assert(linx_lingxi_device_request(&voice, NULL)); execute();
    assert(flash_sets == 1 && !strcmp((char *)flash_config.sta.ssid, "new-network"));

    reset(); begin_wifi(LINX_VOICE_WIFI_SAVE);
    linx_voice_request_t open = {.command = LINX_VOICE_WIFI_SAVE, .generation = 7};
    strcpy(open.wifi_network.ssid, "open-network"); open.wifi_network.security = LINX_WIFI_OPEN;
    assert(linx_lingxi_device_request(&open, NULL)); execute();
    assert(!strcmp((char *)flash_config.sta.ssid, "open-network") && flash_config.sta.password[0] == 0);

    reset(); begin_wifi(LINX_VOICE_WIFI_SAVE);
    linx_voice_request_t maximum = {.command = LINX_VOICE_WIFI_SAVE, .generation = 7};
    memset(maximum.wifi_network.ssid, 's', 32); maximum.wifi_network.security = LINX_WIFI_PERSONAL;
    memset(maximum.wifi_password, 'a', 64);
    assert(linx_lingxi_device_request(&maximum, NULL)); execute();
    assert(!memcmp(flash_config.sta.ssid, maximum.wifi_network.ssid, 32));
    assert(!memcmp(flash_config.sta.password, maximum.wifi_password, 64));
    memset(&maximum, 0, sizeof(maximum));

    for (unsigned invalid = 0; invalid < 7; ++invalid) {
        reset(); begin_wifi(LINX_VOICE_WIFI_SAVE);
        linx_voice_request_t request = {.command = LINX_VOICE_WIFI_SAVE, .generation = 7};
        strcpy(request.wifi_network.ssid, "candidate"); request.wifi_network.security = LINX_WIFI_PERSONAL;
        strcpy(request.wifi_password, "new-secret");
        if (invalid == 0) memset(request.wifi_network.ssid, 'x', sizeof(request.wifi_network.ssid));
        if (invalid == 1) request.wifi_network.ssid[0] = 0;
        if (invalid == 2) request.wifi_network.security = LINX_WIFI_UNSUPPORTED;
        if (invalid == 3) request.wifi_network.security = (linx_wifi_security_t)99;
        if (invalid == 4) strcpy(request.wifi_password, "short");
        if (invalid == 5) memset(request.wifi_password, 'x', sizeof(request.wifi_password));
        if (invalid == 6) { request.wifi_network.security = LINX_WIFI_OPEN; strcpy(request.wifi_password, "not-empty"); }
        assert(linx_lingxi_device_request(&request, NULL)); memset(&request, 0, sizeof(request));
        execute(); expect_error(LINX_VOICE_NOT_CONFIGURED); assert_old_network(); assert(ram_sets == 0 && flash_sets == 0);
    }

    struct { const char *operation; int index; } failures[] = {
        {"esp_netif_init", 1}, {"esp_wifi_get_config", 1}, {"esp_wifi_set_storage", 2},
        {"esp_wifi_set_config", 1}, {"esp_wifi_start", 1}, {"esp_wifi_connect", 1},
        {"esp_wifi_set_storage", 3}, {"esp_wifi_set_config", 2}, {"esp_wifi_set_config", 3},
        {"esp_wifi_disconnect", 1}, {"esp_event_post", 1}
    };
    for (unsigned i = 0; i < sizeof(failures) / sizeof(*failures); ++i) {
        reset(); begin_wifi(LINX_VOICE_WIFI_SAVE); failure = failures[i].operation; failure_index = failures[i].index;
        execute(); expect_error(LINX_VOICE_WIFI_FAILED); assert_old_network(); assert(event_counts[LINX_VOICE_WIFI_SAVED] == 0);
    }
    reset(); begin_wifi(LINX_VOICE_WIFI_SAVE); wifi_auto_connect = false; disconnect_delay = 1; execute(); expect_error(LINX_VOICE_WIFI_FAILED); assert_old_network(); assert(flash_sets == 0);
    reset(); begin_wifi(LINX_VOICE_WIFI_SAVE); wifi_auto_connect = false; execute(); expect_error(LINX_VOICE_TIMEOUT); assert_old_network(); assert(flash_sets == 0);
    reset(); begin_wifi(LINX_VOICE_WIFI_SAVE); wifi_auto_connect = false; cancel_delay = true; execute(); assert_old_network(); assert(flash_sets == 0 && event_counts[LINX_VOICE_WIFI_SAVED] == 0);
    reset(); begin_wifi(LINX_VOICE_WIFI_SAVE); cancel_on_ram = true; execute(); assert_old_network(); assert(flash_sets == 0 && wifi_stops == 0);
    reset(); begin_wifi(LINX_VOICE_WIFI_SAVE); cancel_on_connect = true; execute(); assert_old_network(); assert(flash_sets == 0);
    reset(); begin_wifi(LINX_VOICE_WIFI_SAVE); cancel_on_flash = true; execute(); assert_old_network(); assert(flash_sets == 2 && event_counts[LINX_VOICE_WIFI_SAVED] == 0);
    reset(); begin(); configured = false; test_ssid = "fallback-net"; test_password = "fallback-secret"; execute();
    assert(!strcmp((char *)flash_config.sta.ssid, "fallback-net") && !strcmp((char *)flash_config.sta.password, "fallback-secret"));
}

static void test_wifi_setup(void)
{
    reset(); begin_wifi(LINX_VOICE_WIFI_SETUP); portal_submissions = 1;
    execute();
    linx_voice_event_t event;
    assert(linx_lingxi_device_poll(&event) && event.state == LINX_VOICE_WIFI_SAVED);
    assert(event_counts[LINX_VOICE_WIFI_PORTAL] == 1 && event_counts[LINX_VOICE_WIFI_NETWORKS] == 0);
    assert(portal_starts == 1 && portal_stops == 1 && ap_sets == 1 && ap_destroys == 1);
    assert(portal_statuses[LINX_WIFI_PORTAL_CONNECTING] == 1 && portal_statuses[LINX_WIFI_PORTAL_SAVED] == 1);
    assert(portal_statuses[LINX_WIFI_PORTAL_ERROR] == 0 && disconnects == 2);
    assert(clock_us >= 3000000 && !strcmp((char *)flash_config.sta.ssid, "phone-network"));
    assert(ram_sets == 2 && flash_sets == 1 && nvs_closes == 0 && audio_reads == 0);

    reset(); begin_wifi(LINX_VOICE_WIFI_SETUP); portal_submissions = 2; portal_failures = 1;
    old_ip_after_disconnect = true; execute();
    assert(portal_statuses[LINX_WIFI_PORTAL_ERROR] == 1 && portal_statuses[LINX_WIFI_PORTAL_SAVED] == 1);
    assert(connect_calls == 2 && disconnects == 3 && flash_sets == 1 && portal_starts == 1 && portal_stops == 1);
    assert(delayed_disconnects == 3 && barrier_posts == 3 && barriers_dispatched == 3);
    assert(event_counts[LINX_VOICE_WIFI_PORTAL] == 2 && event_counts[LINX_VOICE_WIFI_SAVED] == 1);

    /* 第一轮坏 RAM 配置不能成为第二轮 Flash 写入失败时的回滚来源。 */
    reset(); begin_wifi(LINX_VOICE_WIFI_SETUP); portal_submissions = 2; portal_failures = 1;
    failure = "esp_wifi_set_config"; failure_index = 5;
    execute(); expect_error(LINX_VOICE_TIMEOUT); assert_old_network();
    assert(portal_statuses[LINX_WIFI_PORTAL_ERROR] == 2 && portal_statuses[LINX_WIFI_PORTAL_SAVED] == 0);
    assert(portal_starts == 1 && portal_stops == 1 && flash_sets == 0);

    struct {const char *operation; int index; linx_voice_error_t error;} failures[] = {
        {"nvs_flash_init", 1, LINX_VOICE_WIFI_FAILED}, {"esp_wifi_get_config", 1, LINX_VOICE_WIFI_FAILED},
        {"esp_wifi_set_storage", 2, LINX_VOICE_WIFI_FAILED}, {"esp_wifi_start", 1, LINX_VOICE_WIFI_FAILED},
        {"esp_wifi_scan_start", 1, LINX_VOICE_WIFI_FAILED}, {"esp_netif_new", 2, LINX_VOICE_NO_MEMORY},
        {"esp_netif_attach_wifi_ap", 1, LINX_VOICE_WIFI_FAILED}, {"esp_wifi_set_default_wifi_ap_handlers", 1, LINX_VOICE_WIFI_FAILED},
        {"esp_wifi_set_mode", 2, LINX_VOICE_WIFI_FAILED}, {"esp_read_mac", 1, LINX_VOICE_WIFI_FAILED},
        {"esp_wifi_set_config", 1, LINX_VOICE_WIFI_FAILED}, {"linx_wifi_portal_start", 1, LINX_VOICE_WIFI_FAILED}
    };
    for (unsigned i = 0; i < sizeof(failures) / sizeof(*failures); ++i) {
        reset(); begin_wifi(LINX_VOICE_WIFI_SETUP);
        failure = failures[i].operation; failure_index = failures[i].index;
        execute(); expect_error(failures[i].error); assert_old_network();
        assert(portal_stops == 0 && event_counts[LINX_VOICE_WIFI_SAVED] == 0);
    }
    reset(); begin_wifi(LINX_VOICE_WIFI_SETUP); failure = "linx_wifi_portal_start"; failure_code = ESP_ERR_NO_MEM; execute(); expect_error(LINX_VOICE_NO_MEMORY);
    reset(); begin_wifi(LINX_VOICE_WIFI_SETUP); existing_ap_netif = true; execute(); expect_error(LINX_VOICE_WIFI_FAILED); assert(ap_destroys == 0);
    reset(); begin_wifi(LINX_VOICE_WIFI_SETUP); portal_time_out = true; execute(); expect_error(LINX_VOICE_TIMEOUT); assert(portal_stops == 1 && ap_destroys == 1);
    reset(); begin_wifi(LINX_VOICE_WIFI_SETUP); portal_cancel = true; execute(); assert(event_counts[LINX_VOICE_WIFI_SAVED] == 0 && portal_stops == 1); assert_old_network();
    reset(); begin_wifi(LINX_VOICE_WIFI_SETUP); portal_submissions = 1; portal_cancel_success = true; execute(); assert(portal_statuses[LINX_WIFI_PORTAL_SAVED] == 1 && event_counts[LINX_VOICE_WIFI_SAVED] == 0 && portal_stops == 1);
    reset(); begin_wifi(LINX_VOICE_WIFI_SETUP); portal_submissions = 1; ignore_association = true; execute(); expect_error(LINX_VOICE_TIMEOUT); assert_old_network(); assert(portal_statuses[LINX_WIFI_PORTAL_ERROR] == 1);
    reset(); begin_wifi(LINX_VOICE_WIFI_SETUP); portal_submissions = 1; unrelated_association = true; execute(); expect_error(LINX_VOICE_TIMEOUT); assert_old_network();
    reset(); begin_wifi(LINX_VOICE_WIFI_SETUP); portal_submissions = 1; disconnect_returns_not_connected = true; execute(); assert(portal_statuses[LINX_WIFI_PORTAL_SAVED] == 1);
    reset(); begin_wifi(LINX_VOICE_WIFI_SETUP); portal_submissions = 1; failure = "esp_wifi_disconnect"; execute(); expect_error(LINX_VOICE_TIMEOUT); assert_old_network();
    reset(); begin_wifi(LINX_VOICE_WIFI_SETUP); portal_submissions = 1; failure = "esp_wifi_set_storage"; failure_index = 3; execute(); expect_error(LINX_VOICE_TIMEOUT); assert(portal_statuses[LINX_WIFI_PORTAL_ERROR] == 1);
    reset(); begin_wifi(LINX_VOICE_WIFI_SETUP); portal_submissions = 1; failure = "esp_event_post"; execute(); expect_error(LINX_VOICE_TIMEOUT); assert_old_network(); assert(connect_calls == 0);
    reset(); begin_wifi(LINX_VOICE_WIFI_SETUP); portal_submissions = 1; failure = "esp_event_handler_instance_register"; failure_index = 3; execute(); expect_error(LINX_VOICE_TIMEOUT); assert_old_network();
    reset(); begin_wifi(LINX_VOICE_WIFI_SETUP); portal_submissions = 1; drop_barrier = true; execute(); expect_error(LINX_VOICE_TIMEOUT); assert_old_network(); assert(connect_calls == 0);
    reset(); begin_wifi(LINX_VOICE_WIFI_SETUP); portal_submissions = 2; portal_failures = 1; drop_retry_disconnect = true; execute(); expect_error(LINX_VOICE_TIMEOUT); assert_old_network(); assert(connect_calls == 1);
    reset(); begin_wifi(LINX_VOICE_WIFI_SETUP); portal_submissions = 1; cancel_delay_at = 2; execute(); assert(portal_stops == 1 && event_counts[LINX_VOICE_WIFI_SAVED] == 0); assert_old_network();
    reset(); begin_wifi(LINX_VOICE_WIFI_SETUP); portal_replace = true; execute(); assert(portal_stops == 1 && ap_destroys == 1 && session_allocations == 2 && task_calls == 1);
    reset(); begin_wifi(LINX_VOICE_WIFI_SETUP); portal_submissions = 1; portal_cancel = true; portal_stop_failures = 3; execute();
    assert(portal_stops == 4 && !pending_cleanup && wifi_mode == WIFI_MODE_STA && event_counts[LINX_VOICE_ERROR] == 0);
    reset(); begin_wifi(LINX_VOICE_WIFI_SETUP); portal_cancel = true; portal_stop_failures = 1; failure = "esp_wifi_set_mode"; failure_index = 3; execute(); assert(portal_stops == 2 && !pending_cleanup && ap_destroys == 1);
    reset(); begin_wifi(LINX_VOICE_WIFI_SETUP); portal_cancel = true; failure = "linx_wifi_portal_stop"; execute(); assert(portal_stops == 2 && !pending_cleanup && ap_destroys == 1 && wifi_mode == WIFI_MODE_STA);
}

static void test_wifi_flash_persistence(void)
{
    /* 全新设备：试连后真正持久化；deinit 丢 RAM，VOICE 必须从 NVS 重载成功。 */
    reset(); begin_wifi(LINX_VOICE_WIFI_SETUP); configured = false; portal_submissions = 1;
    execute();
    assert(flash_sets == 1 && !strcmp((char *)flash_config.sta.ssid, "phone-network"));
    assert(!strcmp((char *)flash_config.sta.password, "phone-secret") && ram_config.sta.ssid[0] == 0);
    linx_voice_event_t event; assert(linx_lingxi_device_poll(&event) && event.state == LINX_VOICE_WIFI_SAVED);
    unsigned previous_errors = event_counts[LINX_VOICE_ERROR];
    linx_voice_request_t voice = {.command = LINX_VOICE_START, .generation = 8};
    assert(linx_lingxi_device_request(&voice, NULL)); execute();
    assert(http_cleanups == 1 && event_counts[LINX_VOICE_ERROR] == previous_errors);
    assert(flash_sets == 1 && !strcmp((char *)flash_config.sta.ssid, "phone-network"));

    /* 同一已保存网络重复配网：同值 SDK fast path 不写 NVS，但原数据仍持久存在。 */
    linx_voice_request_t repeat = {.command = LINX_VOICE_WIFI_SETUP, .generation = 9};
    portal_submissions = 1; assert(linx_lingxi_device_request(&repeat, NULL)); execute();
    assert(linx_lingxi_device_poll(&event) && event.generation == 9 && event.state == LINX_VOICE_WIFI_SAVED);
    assert(flash_sets == 1 && config_noops >= 3);
    voice.generation = 10; assert(linx_lingxi_device_request(&voice, NULL)); execute();
    assert(http_cleanups == 2 && event_counts[LINX_VOICE_ERROR] == previous_errors);

    reset(); begin_wifi(LINX_VOICE_WIFI_SAVE); flash_error_after_ram = true; execute();
    expect_error(LINX_VOICE_WIFI_FAILED); assert_old_network(); assert(flash_sets == 1);
    reset(); begin_wifi(LINX_VOICE_WIFI_SAVE); cancel_on_disconnect = true; execute();
    assert_old_network(); assert(flash_sets == 0 && event_counts[LINX_VOICE_WIFI_SAVED] == 0);
    reset(); begin_wifi(LINX_VOICE_WIFI_SAVE); cancel_ram_at = 2; execute();
    assert_old_network(); assert(flash_sets == 0 && event_counts[LINX_VOICE_WIFI_SAVED] == 0);
    reset(); begin_wifi(LINX_VOICE_WIFI_SAVE); drop_disconnect = true; execute();
    expect_error(LINX_VOICE_TIMEOUT); assert_old_network(); assert(flash_sets == 0);
    reset(); begin_wifi(LINX_VOICE_WIFI_SAVE); drop_barrier = true; execute();
    expect_error(LINX_VOICE_TIMEOUT); assert_old_network(); assert(flash_sets == 0);
}

int main(void)
{
    test_init_and_commands();
    test_success_stream();
    test_audio_resume();
    test_frame_io_timeout();
    test_xiaozhi_minimal_hello();
    test_playback_drain_and_control();
    test_wifi_and_identity_failures();
    test_cold_boot_time();
    test_time_and_gateway();
    test_transport_and_audio_errors();
    test_protocol_sequences_and_cancel();
    test_wifi_scan();
    test_wifi_save();
    test_wifi_setup();
    test_wifi_flash_persistence();
    reset();
    puts("灵矽设备适配器：生命周期、流式音频、取消及失败清理测试通过");
    return 0;
}
