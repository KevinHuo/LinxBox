/* 用真实协议解析器驱动设备 worker；只有 ESP-IDF、Codec 和 BSP 边界被替换。 */
#include <assert.h>
#include <stdbool.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static bool auto_network_ack;
static bool release_network_on_delay;
static char recorded_request_id[513];

static void test_lingxi_log(char level, const char *tag, const char *format, ...)
    __attribute__((format(printf, 3, 4)));
#define ESP_LOGI(tag, ...) test_lingxi_log('I', tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) test_lingxi_log('W', tag, __VA_ARGS__)
#include "sdk_stubs.h"

const char *test_ssid = "";
const char *test_password = "";
static time_t test_time(time_t *out);
static void *test_session_alloc(size_t count, size_t size);
static void test_session_free(void *data);
#define time test_time
#define calloc test_session_alloc
#define free test_session_free
#include "../main/linx_device.c"
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
static unsigned replace_playback_delay_at, stale_cancel_delay_at;
static bool cancel_audio_sleep, upload_stopped;
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
static unsigned event_counts[LINX_VOICE_DRAINING + 1], queue_calls, task_calls, session_allocations, session_frees;
static unsigned wifi_stops, wifi_deinits, netif_destroys, loop_deletes, unregisters, nvs_closes;
static unsigned http_cleanups, ssl_destroys, ws_destroys, ws_closes, sntp_stops;
static unsigned sntp_starts;
static const char *sntp_servers[CONFIG_LWIP_SNTP_MAX_SERVERS];
static int sntp_response_server;
static int64_t sntp_started_us;
static unsigned audio_reads, audio_writes, audio_sleeps, opus_opens, opus_resets, opus_decodes, opus_closes;
static unsigned listen_starts, listen_stops, aborts, audio_sends, pongs, ws_connects;
static unsigned idle_queue_polls, audio_sleep_failures;
static bool enqueue_after_timeout, hard_cancel_requests;
static unsigned handoff_phase;
static linx_voice_command_t handoff_command;
static bool start_on_ready, start_on_http;
static linx_reply_context_t next_context;
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
struct test_tls_error {int esp_code; int tls_code; int flags;};
static struct test_tls_error cached_tls_error;
static int cached_socket_errno;
static bool missing_error_handle;
static unsigned diagnostic_count, tls_error_reads, transport_errno_reads, diagnostic_fault_stage;
static char diagnostics[16][320];
static unsigned protocol_log_count;
static char protocol_logs[128][320];

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
static linx_voice_error_t shutdown_result;
static const char *hello_opus = "{\"type\":\"hello\",\"transport\":\"websocket\",\"session_id\":\"session-1\",\"audio_params\":{\"format\":\"opus\",\"sample_rate\":24000,\"channels\":1}}";
static const char *hello_pcm = "{\"type\":\"hello\",\"transport\":\"websocket\",\"session_id\":\"session-1\",\"audio_params\":{\"format\":\"pcm\",\"sample_rate\":16000,\"channels\":1}}";
static const char *tts_start = "{\"type\":\"tts\",\"state\":\"start\",\"session_id\":\"session-1\"}";
static const char *tts_stop = "{\"type\":\"tts\",\"state\":\"stop\",\"session_id\":\"session-1\"}";

static esp_err_t injected(const char *operation)
{
    if (!failure || strcmp(failure, operation)) return ESP_OK;
    if (diagnostic_fault_stage && (!retained ||
        !(diagnostic_fault_stage == 1 ? retained->waiting : retained->speaking))) return ESP_OK;
    return ++failure_calls == failure_index ? failure_code : ESP_OK;
}
#define INJECT() do { esp_err_t injected_result = injected(__func__); if (injected_result != ESP_OK) return injected_result; } while (0)

void test_task_enter(portMUX_TYPE *lock) { assert(lock->depth == 0); ++lock->depth; }
void test_task_exit(portMUX_TYPE *lock) { assert(lock->depth == 1); --lock->depth; }

static void cancel(void)
{
    linx_voice_request_t request = {.command = hard_cancel_requests ? LINX_VOICE_RELEASE : LINX_VOICE_CANCEL, .generation = atomic_load(&active_generation)};
    assert(linx_device_request(&request, NULL));
}

static void finish(void)
{
    linx_voice_request_t request = {.command = LINX_VOICE_FINISH, .generation = atomic_load(&active_generation)};
    assert(linx_device_request(&request, NULL));
}

static void handoff(unsigned phase)
{
    if (handoff_phase != phase) return;
    handoff_phase = 0;
    linx_voice_request_t request = {.command = handoff_command, .generation = 8, .context = next_context};
    assert(audio_reads == 0 && listen_starts == 0 && !audio_live);
    assert(linx_device_request(&request, NULL));
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
    ++session_frees;
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
        assert(event->state <= LINX_VOICE_DRAINING);
        if (event->state == LINX_VOICE_FINISHED) {
            /* 播放收尾后音频休眠，Wi-Fi/WSS 留给下一条显式指令。 */
            assert(!mock_http.live);
            assert(mock_ssl.live && mock_ws.live);
            assert(wifi_live && wifi_running && netif_live && !audio_live && !codec_live);
            assert(!sntp_running && !pending_cleanup && retained);
            assert(!atomic_load(&listening) && !atomic_load(&finish_pending));
            assert(session_frees + 1 == session_allocations);
        }
        ++event_counts[event->state];
        if (event->state == LINX_VOICE_READY && start_on_ready) {
            start_on_ready = false;
            linx_voice_request_t next = {.command = LINX_VOICE_START, .generation = event->generation + 1U,
                                         .context = next_context};
            assert(audio_reads == 0 && listen_starts == 0 && !audio_live);
            assert(linx_device_request(&next, NULL));
        }
    }
    return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t queue, void *data, TickType_t wait)
{
    assert(queue && data);
    if (!queue->ready) {
        if (wait == READ_TIMEOUT_MS && idle_queue_polls) {
            --idle_queue_polls;
            clock_us += wait * 1000;
            if (enqueue_after_timeout) {
                enqueue_after_timeout = false;
                linx_voice_request_t next = {.command = LINX_VOICE_PREPARE, .generation = 8, .context = next_context};
                assert(linx_device_request(&next, NULL));
            }
            return pdFALSE;
        }
        if (wait == portMAX_DELAY || wait == READ_TIMEOUT_MS) longjmp(worker_end, 1);
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
    if (auto_network_ack) linx_device_network_closed(atomic_load(&network_epoch));
    if (release_network_on_delay) linx_device_network_release();
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
        assert(linx_device_request(&replacement, NULL));
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
    if (replace_playback_delay_at && delay_calls == replace_playback_delay_at) {
        linx_voice_request_t replacement = {.command = LINX_VOICE_START, .generation = 8};
        assert(linx_device_request(&replacement, NULL));
    }
    if (stale_cancel_delay_at && delay_calls == stale_cancel_delay_at) {
        linx_voice_request_t stale = {.command = LINX_VOICE_CANCEL, .generation = 6};
        assert(linx_device_request(&stale, NULL));
    }
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
    handoff(1);
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
    esp_err_t result = client->config.event_handler(&event);
    handoff(2);
    if (start_on_http) {
        start_on_http = false;
        linx_voice_request_t next = {.command = LINX_VOICE_START, .generation = 8, .context = next_context};
        assert(audio_reads == 0 && listen_starts == 0 && !audio_live);
        assert(linx_device_request(&next, NULL));
    }
    return result;
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
    ++ws_connects;
    handoff(3);
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
            if (strstr(buffer, "\"state\":\"start\"")) {
                ++listen_starts; upload_stopped = false;
                const char *id = strstr(buffer, "\"request_id\":\"");
                recorded_request_id[0] = 0;
                if (id) {
                    id += strlen("\"request_id\":\"");
                    const char *end = strchr(id, '"'); assert(end);
                    size_t length = (size_t)(end - id); assert(length < sizeof(recorded_request_id));
                    memcpy(recorded_request_id, id, length); recorded_request_id[length] = 0;
                }
            }
            else { assert(strstr(buffer, "\"state\":\"stop\"")); ++listen_stops; upload_stopped = true; }
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
    if (action == 6 || action == 7) {
        if (action == 7) cancel();
        linx_voice_request_t next = {.command = action == 6 ? LINX_VOICE_START : LINX_VOICE_PREPARE,
                                     .generation = atomic_load(&active_generation) + 1U, .context = next_context};
        assert(linx_device_request(&next, NULL));
    }
    if (action == 10) clock_us += 5000000;
    if (action == 8) {
        linx_voice_request_t stale = {.command = LINX_VOICE_FINISH, .generation = 7};
        assert(linx_device_request(&stale, NULL) && !atomic_load(&finish_pending));
        stale.command = LINX_VOICE_CANCEL;
        assert(linx_device_request(&stale, NULL) && current(8) && atomic_load(&listening));
        finish();
    }
}
int esp_transport_poll_read(esp_transport_handle_t transport, int timeout)
{
    assert(transport == &mock_ws && (timeout == 0 || timeout == 20));
    if (injected(__func__) != ESP_OK) return -1;
    if (poll_empty && timeout == 0) { poll_empty = false; return 0; }
    if (frame_index == frame_count) {
        if (!retained || !retained->hello || retained->turn_active) cancel();
        clock_us += timeout * 1000;
        return 0;
    }
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
    handoff(4);
    last_opcode = frame.action == 11 ? WS_TRANSPORT_OPCODES_NONE : frame.opcode; last_fin = frame.fin; last_payload = frame.payload;
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
esp_tls_error_handle_t esp_transport_get_error_handle(esp_transport_handle_t transport)
{
    assert(transport == &mock_ws && mock_ws.live);
    return missing_error_handle ? NULL : &cached_tls_error;
}
int esp_transport_get_errno(esp_transport_handle_t transport)
{
    assert(transport == &mock_ws && mock_ws.live);
    ++transport_errno_reads;
    int result = cached_socket_errno;
    cached_socket_errno = 0;
    return result;
}
esp_err_t esp_tls_get_and_clear_last_error(esp_tls_error_handle_t error, int *code, int *flags)
{
    assert(error == &cached_tls_error && code && flags && mock_ws.live);
    ++tls_error_reads;
    *code = error->tls_code; *flags = error->flags;
    int result = error->esp_code;
    memset(error, 0, sizeof(*error));
    return result;
}

static void test_lingxi_log(char level, const char *tag, const char *format, ...)
{
    char message[320];
    va_list arguments;
    va_start(arguments, format);
    int size = vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    assert(size >= 0 && (size_t)size < sizeof(message));
    assert(!strstr(message, "secret-token") && !strstr(message, "session-1") && !strstr(message, "Authorization"));
    assert(!strstr(message, "existing-net") && !strstr(message, "ab:ab:ab:ab:ab:ab") && !strstr(message, "diagnostic-private"));
    assert(!strstr(message, "private-text") && !strstr(message, "voice.example.org") && !strstr(message, "real-session-") && !strstr(message, "round-"));
    if (strcmp(tag, "linx_voice")) { assert(!strcmp(tag, "linx_time")); return; }
    if (level == 'I') {
        if (!strncmp(message, "protocol ", 9)) {
            assert(protocol_log_count < sizeof(protocol_logs) / sizeof(protocol_logs[0]));
            strcpy(protocol_logs[protocol_log_count++], message);
        }
        return;
    }
    assert(diagnostic_count < sizeof(diagnostics) / sizeof(diagnostics[0]));
    strcpy(diagnostics[diagnostic_count++], message);
}

esp_log_level_t esp_log_level_get(const char *tag) { assert(!strcmp(tag, "transport_ws")); return log_level; }
void esp_log_level_set(const char *tag, esp_log_level_t level) { assert(!strcmp(tag, "transport_ws")); log_level = level; }

esp_err_t bsp_audio_init(void) { INJECT(); if (!audio_initialized) audio_live = true; audio_initialized = true; return ESP_OK; }
esp_err_t bsp_audio_set_format(uint32_t rate, uint8_t bits, uint8_t channels) { assert(rate == 16000 && bits == 16 && channels == 1); INJECT(); return audio_live ? ESP_OK : ESP_ERR_INVALID_STATE; }
esp_err_t bsp_audio_wake(void) { INJECT(); audio_live = true; return ESP_OK; }
esp_err_t bsp_audio_sleep(void)
{
    ++audio_sleeps;
    if (cancel_audio_sleep) cancel();
    if (audio_sleep_failures) { --audio_sleep_failures; return ESP_FAIL; }
    INJECT();
    audio_live = false;
    return ESP_OK;
}
void bsp_audio_set_volume(uint8_t volume) { assert(volume == 50); }
esp_err_t bsp_audio_read(void *data, size_t size) { assert(audio_live && size == 640 && !upload_stopped); INJECT(); ++audio_reads; memset(data, 0x12, size); if (cancel_audio_read) cancel(); return ESP_OK; }
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
        assert(linx_device_request(&next, NULL));
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
    assert(!mock_http.live && !mock_ssl.live && !mock_ws.live && !sntp_running && !pending_cleanup && !retained);
    if (commands) vQueueDelete(commands);
    if (events) vQueueDelete(events);
    commands = NULL; events = NULL;
    atomic_store(&active, false); atomic_store(&active_generation, 0);
    atomic_store(&listening, false); atomic_store(&finish_pending, false);
    atomic_store(&discard_transport, false);
    atomic_store(&cancel_turn_pending, false);
    atomic_store(&network_wanted, false); atomic_store(&network_ready, false);
    atomic_store(&network_provisioning, false); atomic_store(&network_epoch, 0);
    atomic_store(&network_close_ack, 0); atomic_store(&network_error, LINX_VOICE_OK);
    memset(network_mac, 0, sizeof(network_mac)); network_retry_at = 0;
    auto_network_ack = release_network_on_delay = false; recorded_request_id[0] = 0;
    memset(&requested_target, 0, sizeof(requested_target));
    audio_initialized = false;
    shutdown_result = LINX_VOICE_OK;
    failure = NULL; failure_code = ESP_FAIL; failure_index = 1; failure_calls = 0;
    session_oom = existing_loop = existing_netif = false; wifi_auto_connect = true;
    nvs_missing = true; nvs_invalid = false; configured = true; sntp_existing = false;
    cancel_delay = cancel_perform = cancel_decode = cancel_audio_read = false;
    cancel_audio_sleep = upload_stopped = false;
    replace_playback_delay_at = stale_cancel_delay_at = 0;
    disconnect_delay = time_ready_delay = 0; delay_calls = 0;
    clock_us = 0; wall_time = 1800000000;
    strcpy(persisted_uuid, "12345678-1234-4123-8123-123456789012");
    memset(event_counts, 0, sizeof(event_counts));
    queue_calls = task_calls = session_allocations = session_frees = 0;
    wifi_stops = wifi_deinits = netif_destroys = loop_deletes = unregisters = nvs_closes = 0;
    http_cleanups = ssl_destroys = ws_destroys = ws_closes = sntp_stops = 0;
    sntp_starts = 0; sntp_response_server = -1; sntp_started_us = 0;
    memset(sntp_servers, 0, sizeof(sntp_servers));
    audio_reads = audio_writes = audio_sleeps = opus_opens = opus_resets = opus_decodes = opus_closes = 0;
    listen_starts = listen_stops = aborts = audio_sends = pongs = ws_connects = 0;
    idle_queue_polls = audio_sleep_failures = handoff_phase = 0;
    handoff_command = LINX_VOICE_PREPARE; start_on_ready = start_on_http = enqueue_after_timeout = hard_cancel_requests = false;
    memset(&next_context, 0, sizeof(next_context));
    decode_fault = 0; http_status = 200;
    http_body = "{\"websocket\":{\"url\":\"wss://voice.example.org/chat\",\"token\":\"secret-token\"}}";
    http_negative = http_overflow = poll_empty = false;
    log_level = 3; handler_count = 0;
    frame_count = frame_index = 0;
    last_opcode = WS_TRANSPORT_OPCODES_NONE; last_fin = true; last_payload = 0;
    cached_tls_error = (struct test_tls_error){0}; cached_socket_errno = 0; missing_error_handle = false;
    diagnostic_count = tls_error_reads = transport_errno_reads = diagnostic_fault_stage = 0;
    memset(diagnostics, 0, sizeof(diagnostics));
    protocol_log_count = 0; memset(protocol_logs, 0, sizeof(protocol_logs));
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
    assert(linx_device_init());
    assert(queue_calls == 2 && task_calls == 1 && session_allocations == 0);
    assert(!wifi_live && !audio_live);
    linx_voice_request_t request = {.command = LINX_VOICE_START, .generation = 7};
    assert(linx_device_request(&request, NULL));
}
/* 测试在 worker 阻塞等命令时显式关闭，生产没有退出/销毁队列 API。 */
static void execute_retained(void)
{
    if (!setjmp(worker_end)) worker_function(NULL);
}
static void shutdown_retained(void)
{
    if (retained) {
        shutdown_result = cleanup(retained);
    }
}
static void execute(void)
{
    execute_retained();
    shutdown_retained();
    assert(log_level == 3);
    assert(!mock_http.live && !mock_ssl.live && !mock_ws.live);
    assert(!wifi_live && !wifi_running && !netif_live && !loop_live && !audio_live && !codec_live);
}
static void expect_error_at(linx_voice_error_t error, int line)
{
    linx_voice_event_t event;
    assert(linx_device_poll(&event));
    if (event.state != LINX_VOICE_ERROR || event.error != error) {
        fprintf(stderr, "line %d: expected error %d, actual state %d error %d (injection %s)\n", line, error, event.state, event.error, failure ? failure : "none");
        assert(false);
    }
    assert(event.generation == 7);
    assert(!linx_device_poll(&event));
}

#define expect_error(error) expect_error_at(error, __LINE__)

static void test_init_and_commands(void)
{
    reset();
    assert(!linx_device_poll(NULL));
    linx_voice_event_t event;
    linx_voice_request_t request = {.command = LINX_VOICE_START, .generation = 7};
    assert(!linx_device_poll(&event));
    request.command = LINX_VOICE_FINISH;
    assert(!linx_device_request(&request, NULL));
    request.command = LINX_VOICE_START;
    failure = "xQueueCreate"; failure_index = 1;
    assert(!linx_device_init() && !commands && !events);
    failure_calls = 0; failure_index = 2;
    assert(!linx_device_init() && !commands && !events);
    failure = "xTaskCreate"; failure_calls = 0; failure_index = 1;
    assert(!linx_device_init() && !commands && !events);
    failure_calls = 0;
    assert(!linx_device_request(&request, NULL) && !commands && !events);
    failure = NULL;
    assert(linx_device_request(&request, NULL) && commands && events);
    cancel(); execute();
    reset(); begin(); assert(linx_device_init() && task_calls == 1);
    assert(!linx_device_request(NULL, NULL));
    request.command = (linx_voice_command_t)-1;
    assert(!linx_device_request(&request, NULL));
    request.command = (linx_voice_command_t)99;
    assert(!linx_device_request(&request, NULL));
    request.command = LINX_VOICE_FINISH; request.generation = 3;
    assert(linx_device_request(&request, NULL) && !atomic_load(&finish_pending));
    request.generation = 7;
    assert(linx_device_request(&request, NULL) && !atomic_load(&finish_pending));
    cancel(); execute();
    assert(session_allocations == 0 && linx_device_poll(&event) && event.state == LINX_VOICE_CLOSED);
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
    /* 完成本条后不能消费下一轮命令，也不能重新开启麦克风。 */
    add_wait(2);
    execute();
    assert(listen_starts == 1 && listen_stops == 1 && aborts == 0);
    assert(audio_sends == 1 && audio_reads == 1 && audio_writes == 1);
    assert(opus_opens == 1 && opus_resets == 1 && opus_decodes == 1 && opus_closes == 1 && audio_sleeps == 1);
    assert(http_cleanups == 1 && ws_closes == 1 && ws_destroys == 1 && ssl_destroys == 1);
    assert(wifi_stops == 1 && wifi_deinits == 1 && netif_destroys == 1 && unregisters == 2 && loop_deletes == 1);
    assert(event_counts[LINX_VOICE_LISTENING] == 1 && event_counts[LINX_VOICE_WAITING] == 1 && event_counts[LINX_VOICE_SPEAKING] == 1);
    assert(event_counts[LINX_VOICE_FINISHED] == 1 && event_counts[LINX_VOICE_ERROR] == 0);
    assert(delay_calls == 6 && frame_index == frame_count - 1);
    assert(persisted_uuid[14] == '4');
    linx_voice_event_t event; assert(linx_device_poll(&event) && event.state == LINX_VOICE_FINISHED);
    assert(event.error == LINX_VOICE_OK && event.generation == 7);
    assert(!linx_device_poll(&event));
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
    reset(); begin(); wall_time = 0; cancel_delay_at = 800; hard_cancel_requests = true; execute();
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
        failure = cleanup_operations[i]; execute(); assert(shutdown_result == LINX_VOICE_WIFI_FAILED);
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
    reset(); begin(); cancel_perform = true; hard_cancel_requests = true; execute(); assert(ssl_destroys == 0 && http_cleanups == 1 && event_counts[LINX_VOICE_ERROR] == 0);
    reset(); begin(); http_body = "{\"activation\":{\"code\":\"123456\"}}"; execute();
    linx_voice_event_t event; assert(linx_device_poll(&event) && event.state == LINX_VOICE_BINDING && !strcmp(event.activation_code, "123456"));
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
    reset(); begin(); add_text(hello_opus); add_wait(1); add_text(tts_start); add_binary("opus", 4); failure = "esp_opus_dec_decode"; failure_code = ESP_AUDIO_ERR_MEM_LACK; execute(); expect_error(LINX_VOICE_NO_MEMORY);
    for (unsigned fault = 1; fault <= 7; ++fault) { reset(); begin(); add_text(hello_opus); add_wait(1); add_text(tts_start); add_binary("opus", 4); decode_fault = fault; execute(); expect_error(LINX_VOICE_AUDIO_FAILED); assert(audio_writes == 0); }
    reset(); begin(); add_text(hello_opus); add_wait(1); add_text(tts_start); add_binary("opus", 4); cancel_decode = true; execute(); assert(opus_decodes == 1 && audio_writes == 0 && event_counts[LINX_VOICE_FINISHED] == 0);
    reset(); begin(); voice_session_t *session = test_session_alloc(1, sizeof(*session)); session->audio_awake = true; failure = "bsp_audio_sleep"; assert(cleanup(session) == LINX_VOICE_AUDIO_FAILED); cancel(); execute();
    reset(); begin(); add_text(hello_opus); cancel_audio_read = true; execute(); assert(audio_reads == 1 && audio_sends == 0);
    reset(); begin(); add_text(hello_pcm); add_wait(1); add_text(tts_start); static uint8_t pcm[640]; add_binary(pcm, sizeof(pcm)); add_text(tts_stop); execute(); assert(opus_opens == 0 && audio_writes == 1 && listen_starts == 1 && event_counts[LINX_VOICE_FINISHED] == 1);
    reset(); begin(); add_text(hello_pcm); add_wait(1); add_text(tts_start); add_binary("odd", 3); execute(); expect_error(LINX_VOICE_PROTOCOL_FAILED);
    reset(); begin(); add_text(hello_pcm); add_wait(1); add_text(tts_start); static uint8_t excessive[3842]; add_binary(excessive, sizeof(excessive)); execute(); expect_error(LINX_VOICE_PROTOCOL_FAILED);
}

static void test_protocol_sequences_and_cancel(void)
{
    reset(); begin(); add_text("not JSON"); execute(); expect_error(LINX_VOICE_PROTOCOL_FAILED);
    reset(); begin(); add_text(tts_start); execute(); expect_error(LINX_VOICE_PROTOCOL_FAILED);
    reset(); begin(); add_binary("opus", 4); execute(); expect_error(LINX_VOICE_PROTOCOL_FAILED);
    reset(); begin(); add_text(hello_opus); add_text(hello_opus); execute(); expect_error(LINX_VOICE_PROTOCOL_FAILED);
    reset(); begin(); add_text(hello_opus); add_text("{\"type\":\"tts\",\"state\":\"start\",\"session_id\":\"other\"}"); execute(); expect_error(LINX_VOICE_PROTOCOL_FAILED);
    reset(); begin(); add_text(hello_opus); add_text(tts_stop); execute(); expect_error(LINX_VOICE_PROTOCOL_FAILED);
    reset(); begin(); add_text(hello_opus); add_wait(1); add_text(tts_start); add_text("{\"type\":\"tts\",\"state\":\"stop\",\"is_aborted\":true}"); execute(); expect_error(LINX_VOICE_PROTOCOL_FAILED);
    assert(listen_starts == 1 && audio_sleeps == 1 && event_counts[LINX_VOICE_FINISHED] == 0);
    reset(); begin(); add_text(hello_opus); add_text(tts_start); add_text(tts_stop); execute(); expect_error(LINX_VOICE_PROTOCOL_FAILED);
    assert(listen_stops == 0 && event_counts[LINX_VOICE_FINISHED] == 0);
    reset(); begin(); add_text(hello_opus); add_wait(1); add_text(tts_start); add_text(tts_stop);
    failure = "esp_transport_ws_send_raw"; failure_index = 4;
    execute(); expect_error(LINX_VOICE_NETWORK_FAILED);
    assert(diagnostic_count == 1 && strstr(diagnostics[0], "operation=send phase=recording"));
    assert(listen_stops == 0 && event_counts[LINX_VOICE_WAITING] == 0 && event_counts[LINX_VOICE_FINISHED] == 0);
    reset(); begin(); add_text(hello_opus); add_wait(1); add_text(tts_start); add_text(tts_start); execute(); expect_error(LINX_VOICE_PROTOCOL_FAILED);
    reset(); begin(); frames[frame_count++] = (frame_t){.opcode = 8, .fin = true}; execute(); expect_error(LINX_VOICE_NETWORK_FAILED);
    reset(); begin(); frames[frame_count++] = (frame_t){.opcode = 1, .fin = true, .payload = -1}; execute(); expect_error(LINX_VOICE_PROTOCOL_FAILED);
    reset(); begin(); frames[frame_count++] = (frame_t){.opcode = 9, .fin = true}; frames[frame_count++] = (frame_t){.opcode = 10, .fin = true}; add_text(hello_opus); execute(); assert(pongs == 1 && listen_starts == 1);
    reset(); begin(); frames[frame_count++] = (frame_t){.opcode = 1, .fin = true, .payload = 4097, .size = 1, .data = "x"}; execute(); expect_error(LINX_VOICE_PROTOCOL_FAILED);
    reset(); begin(); add_wait(4); execute(); expect_error(LINX_VOICE_TIMEOUT);
    reset(); begin(); add_text(hello_opus); add_wait(1); add_wait(4); execute(); expect_error(LINX_VOICE_TIMEOUT);
    reset(); begin(); add_text(hello_opus); add_wait(1); add_text(tts_start); add_wait(4); execute(); expect_error(LINX_VOICE_TIMEOUT);
    reset(); begin(); add_text(hello_opus); add_wait(3); execute(); expect_error(LINX_VOICE_WIFI_FAILED);
    reset(); begin(); add_text(hello_opus); poll_empty = true; execute(); assert(audio_reads == 2 && audio_sends == 2);
    reset(); begin(); frames[frame_count++] = (frame_t){.opcode = 9, .fin = false}; execute(); expect_error(LINX_VOICE_PROTOCOL_FAILED);
    reset(); begin(); frames[frame_count++] = (frame_t){.opcode = 9, .fin = true, .payload = 126}; execute(); expect_error(LINX_VOICE_PROTOCOL_FAILED);
    reset(); begin(); frames[frame_count++] = (frame_t){.opcode = 9, .fin = true, .payload = 1}; execute(); expect_error(LINX_VOICE_NETWORK_FAILED);
    reset(); begin(); add_text(hello_opus); add_wait(1); add_wait(1); add_text(tts_start); frames[frame_count - 1].action = 1;
    add_text(tts_stop); frames[frame_count - 1].action = 1;
    execute(); assert(listen_starts == 1 && listen_stops == 1 && event_counts[LINX_VOICE_FINISHED] == 1);
    reset(); begin(); linx_voice_event_t stale = {.generation = 2, .state = LINX_VOICE_ERROR}; xQueueOverwrite(events, &stale);
    linx_voice_event_t event; assert(!linx_device_poll(&event)); cancel();
    stale.generation = 7; xQueueOverwrite(events, &stale); assert(!linx_device_poll(&event)); execute();
    reset(); begin(); linx_voice_request_t replacement = {.command = LINX_VOICE_START, .generation = 8};
    assert(linx_device_request(&replacement, NULL));
    linx_voice_request_t late = {.command = LINX_VOICE_CANCEL, .generation = 7};
    assert(linx_device_request(&late, NULL) && current(8));
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
    add_wait(1);
    add_text("{\"type\":\"tts\",\"state\":\"start\"}");
    add_binary("opus", 4);
    add_text("{\"type\":\"tts\",\"state\":\"stop\"}");
    execute();
    assert(listen_starts == 1 && opus_opens == 1 && opus_decodes == 1 && audio_writes == 1);
    assert(event_counts[LINX_VOICE_ERROR] == 0 && event_counts[LINX_VOICE_FINISHED] == 1);
}

static void test_frame_io_timeout(void)
{
    reset(); begin(); add_text(hello_opus); frames[0].action = 5;
    add_wait(1);
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
    for (unsigned at = 1; at <= 6; ++at) {
        reset(); begin(); add_text(hello_opus); add_wait(1); add_text(tts_start); add_text(tts_stop);
        cancel_delay_at = at; execute();
        assert(listen_starts == 1 && listen_stops == 1 && delay_calls == at);
        assert(audio_reads == 1 && event_counts[LINX_VOICE_FINISHED] == 0 && event_counts[LINX_VOICE_ERROR] == 0);
        linx_voice_event_t event; assert(linx_device_poll(&event) && event.state == LINX_VOICE_CLOSED);
    }
    /* 最后一次尾音等待中断网也不能对外报告完成。 */
    reset(); begin(); add_text(hello_opus); add_wait(1); add_text(tts_start); add_text(tts_stop);
    disconnect_delay = 6; execute(); expect_error(LINX_VOICE_WIFI_FAILED);
    assert(delay_calls == 6 && event_counts[LINX_VOICE_FINISHED] == 0);
    reset(); begin(); add_text(hello_opus); add_wait(1); add_text(tts_start); add_text(tts_stop);
    stale_cancel_delay_at = 1; execute();
    assert(listen_starts == 1 && listen_stops == 1 && delay_calls == 6 && event_counts[LINX_VOICE_FINISHED] == 1);
    /* 替换发生在最后一次等待：旧页面不能抢先收到完成；新会话只开启一次监听。 */
    reset(); begin(); add_text(hello_opus); add_wait(1); add_text(tts_start); add_text(tts_stop);
    add_wait(0); add_wait(1); add_text(tts_start); add_text(tts_stop);
    replace_playback_delay_at = 6; execute();
    assert(session_allocations == 1 && listen_starts == 2 && listen_stops == 2 && task_calls == 1);
    assert(delay_calls == 12 && audio_reads == 2 && event_counts[LINX_VOICE_FINISHED] == 1);
    linx_voice_event_t event; assert(linx_device_poll(&event) && event.state == LINX_VOICE_FINISHED && event.generation == 8);
    /* 结束上传后不再读取麦克风，包括旧实现用于恢复监听的五帧清空。 */
    reset(); begin(); add_text(hello_opus); add_wait(1); add_text(tts_start); add_text(tts_stop);
    failure = "bsp_audio_read"; failure_index = 2; execute();
    assert(failure_calls == 1 && audio_reads == 1 && event_counts[LINX_VOICE_FINISHED] == 1);
    const char *cleanup_operations[] = {"bsp_audio_sleep", "esp_wifi_stop", "esp_wifi_deinit", "esp_event_loop_delete_default"};
    for (unsigned i = 0; i < sizeof(cleanup_operations) / sizeof(*cleanup_operations); ++i) {
        reset(); begin(); add_text(hello_opus); add_wait(1); add_text(tts_start); add_text(tts_stop);
        failure = cleanup_operations[i]; execute();
        if (i == 0) {
            expect_error(LINX_VOICE_AUDIO_FAILED);
            assert(event_counts[LINX_VOICE_FINISHED] == 0);
        } else {
            assert(shutdown_result == LINX_VOICE_WIFI_FAILED);
            assert(event_counts[LINX_VOICE_FINISHED] == 1);
        }
        assert(listen_starts == 1 && audio_sleeps == (i == 0 ? 2U : 1U));
    }
    reset(); begin(); add_text(hello_opus); add_wait(1); add_text(tts_start); add_text(tts_stop);
    cancel_audio_sleep = true; execute();
    assert(event_counts[LINX_VOICE_FINISHED] == 0 && event_counts[LINX_VOICE_ERROR] == 0);
    assert(linx_device_poll(&event) && event.state == LINX_VOICE_CLOSED);
}

static void test_audio_resume(void)
{
    reset(); begin(); add_text(hello_opus); add_wait(2); execute();
    assert(audio_initialized && !audio_live && audio_sleeps == 1);
    assert(bsp_audio_set_format(16000, 16, 1) == ESP_ERR_INVALID_STATE);
    frame_count = frame_index = 0;
    linx_voice_request_t request = {.command = LINX_VOICE_START, .generation = 8};
    assert(linx_device_request(&request, NULL));
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
    assert(linx_device_request(&request, NULL));
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
    linx_voice_event_t event; assert(linx_device_poll(&event));
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
    assert(linx_device_poll(&event) && event.state == LINX_VOICE_WIFI_NETWORKS && event.wifi_count == 0);
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
    assert(linx_device_poll(&event) && event.generation == 8 && event.state == LINX_VOICE_WIFI_SAVED);
    assert(!strcmp((char *)flash_config.sta.ssid, "next-network"));
}

static void test_wifi_save(void)
{
    reset(); begin_wifi(LINX_VOICE_WIFI_SAVE); execute();
    linx_voice_event_t event; assert(linx_device_poll(&event));
    assert(event.state == LINX_VOICE_WIFI_SAVED && event_counts[LINX_VOICE_WIFI_CONNECTING] == 1);
    assert(!strcmp((char *)flash_config.sta.ssid, "new-network") && !strcmp((char *)flash_config.sta.password, "new-secret"));
    assert(ram_sets == 2 && flash_sets == 1 && http_cleanups == 0 && nvs_closes == 0);
    assert(!wifi_live && audio_reads == 0 && audio_writes == 0);
    /* NVS 网络优先于编译时设置。 */
    test_ssid = "compile-fallback"; test_password = "fallback-secret";
    linx_voice_request_t voice = {.command = LINX_VOICE_START, .generation = 8};
    assert(linx_device_request(&voice, NULL)); execute();
    assert(flash_sets == 1 && !strcmp((char *)flash_config.sta.ssid, "new-network"));

    reset(); begin_wifi(LINX_VOICE_WIFI_SAVE);
    linx_voice_request_t open = {.command = LINX_VOICE_WIFI_SAVE, .generation = 7};
    strcpy(open.wifi_network.ssid, "open-network"); open.wifi_network.security = LINX_WIFI_OPEN;
    assert(linx_device_request(&open, NULL)); execute();
    assert(!strcmp((char *)flash_config.sta.ssid, "open-network") && flash_config.sta.password[0] == 0);

    reset(); begin_wifi(LINX_VOICE_WIFI_SAVE);
    linx_voice_request_t maximum = {.command = LINX_VOICE_WIFI_SAVE, .generation = 7};
    memset(maximum.wifi_network.ssid, 's', 32); maximum.wifi_network.security = LINX_WIFI_PERSONAL;
    memset(maximum.wifi_password, 'a', 64);
    assert(linx_device_request(&maximum, NULL)); execute();
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
        assert(linx_device_request(&request, NULL)); memset(&request, 0, sizeof(request));
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
    assert(linx_device_poll(&event) && event.state == LINX_VOICE_WIFI_SAVED);
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
    linx_voice_event_t event; assert(linx_device_poll(&event) && event.state == LINX_VOICE_WIFI_SAVED);
    unsigned previous_errors = event_counts[LINX_VOICE_ERROR];
    linx_voice_request_t voice = {.command = LINX_VOICE_START, .generation = 8};
    assert(linx_device_request(&voice, NULL)); execute();
    assert(http_cleanups == 1 && event_counts[LINX_VOICE_ERROR] == previous_errors);
    assert(flash_sets == 1 && !strcmp((char *)flash_config.sta.ssid, "phone-network"));

    /* 同一已保存网络重复配网：同值 SDK fast path 不写 NVS，但原数据仍持久存在。 */
    linx_voice_request_t repeat = {.command = LINX_VOICE_WIFI_SETUP, .generation = 9};
    portal_submissions = 1; assert(linx_device_request(&repeat, NULL)); execute();
    assert(linx_device_poll(&event) && event.generation == 9 && event.state == LINX_VOICE_WIFI_SAVED);
    assert(flash_sets == 1 && config_noops >= 3);
    voice.generation = 10; assert(linx_device_request(&voice, NULL)); execute();
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

static const linx_reply_context_t cached_context = {.scene_id = 1, .task_id = 12,
                                                   .source_turn_id = 23, .image_id = 34};

static void request_voice(linx_voice_command_t command, uint32_t generation, linx_reply_context_t context)
{
    linx_voice_request_t request = {.command = command, .generation = generation, .context = context};
    assert(linx_device_request(&request, NULL));
}

static void prewarm(void)
{
    request_voice(LINX_VOICE_PREPARE, 7, cached_context);
    add_text(hello_opus);
    execute_retained();
    assert(retained && retained->hello && retained->idle_deadline > clock_us);
    assert(wifi_running && mock_ws.live && !audio_live && !codec_live);
    assert(audio_reads == 0 && audio_writes == 0 && audio_sends == 0 && listen_starts == 0 && opus_opens == 0);
    assert(event_counts[LINX_VOICE_READY] == 1 && connect_calls == 1 && http_cleanups == 1 && ws_connects == 1);
    assert(event_counts[LINX_VOICE_CONNECTING] == 1 && event_counts[LINX_VOICE_PREPARING] == 0);
}

static void add_reply(unsigned finish_action)
{
    add_wait(finish_action);
    add_text(tts_start);
    add_binary("opus", 4);
    add_text(tts_stop);
}

static void clear_frames(void)
{
    frame_count = frame_index = 0;
}

static void test_prepare_and_reuse(void)
{
    reset(); prewarm();
    voice_session_t *owner = retained;
    int64_t prepared_deadline = owner->idle_deadline;
    /* 队列的空闲轮询处理分段 PING，不唤醒音频，也不延长 30 秒。 */
    clear_frames();
    frames[frame_count++] = (frame_t){.opcode = 9, .fin = true, .payload = 2, .data = "hi", .size = 1};
    frames[frame_count++] = (frame_t){.opcode = 9, .fin = true, .payload = 2, .data = "i", .size = 1};
    idle_queue_polls = 2; execute_retained();
    assert(pongs == 1 && owner->idle_deadline == prepared_deadline && !audio_live && audio_reads == 0);
    for (uint32_t generation = 8; generation <= 9; ++generation) {
        clear_frames(); add_wait(0); add_reply(generation == 8 ? 8 : 1);
        request_voice(LINX_VOICE_START, generation, cached_context); execute_retained();
        assert(retained == owner && owner->hello && !owner->turn_active && !audio_live && !codec_live);
        assert(event_counts[LINX_VOICE_FINISHED] == generation - 7U && listen_starts == generation - 7U);
        assert(audio_sleeps == generation - 7U && connect_calls == 1 && http_cleanups == 1 && ws_connects == 1);
        assert(event_counts[LINX_VOICE_CONNECTING] == 1);
        assert(event_counts[LINX_VOICE_PREPARING] == generation - 7U);
    }
    /* 完成回概览立刻换代 PREPARE：复用而不是重新 Hello，不重新发布 FINISHED。 */
    clear_frames();
    int64_t finished_deadline = owner->idle_deadline;
    request_voice(LINX_VOICE_PREPARE, 10, cached_context); execute_retained();
    assert(event_counts[LINX_VOICE_FINISHED] == 2 && event_counts[LINX_VOICE_READY] == 2);
    assert(owner->idle_deadline == finished_deadline && audio_reads == 2 && audio_writes == 2);
    assert(event_counts[LINX_VOICE_CONNECTING] == 1 && event_counts[LINX_VOICE_PREPARING] == 2);
    assert(wifi_stops == 0 && wifi_deinits == 0 && session_allocations == 1 && session_frees == 0);
    shutdown_retained();
}

static void test_idle_expiry_and_rebuild(void)
{
    reset(); prewarm();
    voice_session_t *owner = retained;
    int64_t deadline = owner->idle_deadline;
    clear_frames();
    clock_us = deadline - 20000;
    frames[frame_count++] = (frame_t){.opcode = 9, .fin = true, .payload = 1, .data = "p", .size = 1};
    idle_poll();
    assert(clock_us == deadline && owner->hello && owner->idle_deadline == deadline && pongs == 1);
    idle_poll();
    assert(retained == owner && !owner->ws && !owner->hello && owner->idle_deadline == 0);
    assert(wifi_running && connect_calls == 1 && wifi_stops == 0 && audio_reads == 0 && audio_sleeps == 0);
    clear_frames(); add_text(hello_opus); add_reply(1);
    request_voice(LINX_VOICE_START, 8, cached_context); execute_retained();
    assert(event_counts[LINX_VOICE_FINISHED] == 1 && connect_calls == 1 && http_cleanups == 2 && ws_connects == 2);
    assert(event_counts[LINX_VOICE_CONNECTING] == 2 && event_counts[LINX_VOICE_PREPARING] == 1);
    assert(wifi_stops == 0 && listen_starts == 1);
    shutdown_retained();

    /* 恰好到期收到 START 也重建；不依赖 worker 先观察到过期。 */
    reset(); prewarm(); clock_us = retained->idle_deadline;
    clear_frames(); add_text(hello_opus); add_reply(1);
    request_voice(LINX_VOICE_START, 8, cached_context); execute_retained();
    assert(http_cleanups == 2 && ws_connects == 2 && connect_calls == 1 && event_counts[LINX_VOICE_FINISHED] == 1);
    shutdown_retained();
}

static void test_target_changes(void)
{
    for (unsigned field = 0; field < 4; ++field) {
        reset(); prewarm();
        linx_reply_context_t changed = cached_context;
        if (field == 0) ++changed.scene_id;
        if (field == 1) ++changed.task_id;
        if (field == 2) ++changed.source_turn_id;
        if (field == 3) ++changed.image_id;
        clear_frames(); add_text(hello_opus);
        request_voice(LINX_VOICE_PREPARE, 8, changed); execute_retained();
        assert(retained && same_context(&retained->context, &changed));
        assert(http_cleanups == 2 && ws_connects == 2 && ws_destroys == 1 && connect_calls == 1);
        assert(listen_starts == 0 && audio_reads == 0 && !audio_live);
        shutdown_retained();
    }
}

static void test_invalid_idle_connection(void)
{
    for (unsigned invalid = 0; invalid < 5; ++invalid) {
        reset(); prewarm(); clear_frames();
        if (invalid == 0) frames[frame_count++] = (frame_t){.opcode = 8, .fin = true};
        if (invalid == 1) add_text(tts_start);
        if (invalid == 2) add_binary("opus", 4);
        if (invalid == 3) failure = "esp_transport_poll_read";
        if (invalid == 4) add_text("{\"type\":\"stt\",\"text\":\"late\"}");
        idle_queue_polls = 1; execute_retained();
        assert(retained && !retained->ws && !retained->hello && wifi_running && wifi_stops == 0);
        assert(audio_reads == 0 && audio_writes == 0 && listen_starts == 0);
        failure = NULL; clear_frames(); add_text(hello_opus);
        request_voice(LINX_VOICE_PREPARE, 8, cached_context); execute_retained();
        assert(retained->hello && http_cleanups == 2 && ws_connects == 2 && connect_calls == 1);
        shutdown_retained();
    }
    /* START 前已排队的迟到 TTS 必须在麦克风唤醒前作废旧连接。 */
    reset(); prewarm(); clear_frames(); add_text(tts_start); add_text(hello_opus); add_reply(1);
    request_voice(LINX_VOICE_START, 8, cached_context); execute_retained();
    assert(event_counts[LINX_VOICE_FINISHED] == 1 && event_counts[LINX_VOICE_ERROR] == 0);
    assert(ws_connects == 2 && http_cleanups == 2 && connect_calls == 1 && audio_writes == 1);
    shutdown_retained();
    /* 真正的 Wi-Fi 断线释放所有者；下一次显式启动重新联网。 */
    reset(); prewarm(); clear_frames();
    wifi_callback(callback_user, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, NULL); idle_poll();
    assert(!retained && !wifi_live && !mock_ws.live);
    add_text(hello_opus); request_voice(LINX_VOICE_PREPARE, 8, cached_context); execute_retained();
    assert(retained->hello && connect_calls == 2 && http_cleanups == 2 && audio_reads == 0);
    shutdown_retained();
}

static void test_prepare_start_and_cancel_races(void)
{
    /* START 在网关请求结束时接管预热，保留同一次网关与握手。 */
    reset(); next_context = cached_context; start_on_http = true;
    request_voice(LINX_VOICE_PREPARE, 7, cached_context); add_text(hello_opus); add_reply(1);
    execute_retained();
    assert(event_counts[LINX_VOICE_FINISHED] == 1 && event_counts[LINX_VOICE_READY] == 0);
    assert(http_cleanups == 1 && ws_connects == 1 && connect_calls == 1 && listen_starts == 1 && audio_reads == 1);
    shutdown_retained();

    /* START 在旧 Hello 读取期间抢占，旧帧不得跨代播放。 */
    reset(); next_context = cached_context;
    request_voice(LINX_VOICE_PREPARE, 7, cached_context);
    add_text(hello_opus); frames[0].action = 6; add_reply(1);
    execute_retained();
    assert(http_cleanups == 1 && ws_connects == 1 && connect_calls == 1 && event_counts[LINX_VOICE_FINISHED] == 1);
    assert(listen_starts == 1 && audio_writes == 1);
    shutdown_retained();

    /* READY 发布时立即 START：完整连接能直接晋升，无需第二次建连。 */
    reset(); next_context = cached_context; start_on_ready = true;
    request_voice(LINX_VOICE_PREPARE, 7, cached_context);
    add_text(hello_opus); add_wait(0); add_reply(1); execute_retained();
    assert(event_counts[LINX_VOICE_READY] == 1 && event_counts[LINX_VOICE_FINISHED] == 1);
    assert(connect_calls == 1 && http_cleanups == 1 && ws_connects == 1 && listen_starts == 1);
    shutdown_retained();

    /* CANCEL 的快照被 PREPARE 覆盖，仍应 abort 本条并保留静音排空的 WSS。 */
    reset(); prewarm(); next_context = cached_context; clear_frames();
    add_wait(0); add_wait(7);
    request_voice(LINX_VOICE_START, 8, cached_context); execute_retained();
    assert(retained && retained->hello && retained->generation == 9 && !retained->turn_active);
    assert(event_counts[LINX_VOICE_FINISHED] == 0 && event_counts[LINX_VOICE_READY] == 1 && event_counts[LINX_VOICE_DRAINING] == 1);
    assert(http_cleanups == 1 && ws_connects == 1 && connect_calls == 1 && aborts == 1 && retained->draining);
    assert(audio_reads == 1 && audio_writes == 0 && audio_sleeps == 1 && !audio_live);
    shutdown_retained();

    /* 真正的 RELEASE 即使被 PREPARE 快照覆盖，也必须销毁旧 WSS。 */
    reset(); prewarm(); clear_frames(); hard_cancel_requests = true; cancel(); hard_cancel_requests = false;
    request_voice(LINX_VOICE_PREPARE, 8, cached_context); add_text(hello_opus); execute_retained();
    assert(http_cleanups == 2 && ws_connects == 2 && connect_calls == 1 && audio_reads == 0);
    shutdown_retained();
}

static void test_retained_failures_and_provisioning(void)
{
    reset(); prewarm(); clear_frames(); add_wait(0); add_reply(1);
    failure = "bsp_audio_read";
    request_voice(LINX_VOICE_START, 8, cached_context); execute_retained();
    assert(retained && !retained->ws && wifi_running && wifi_stops == 0 && !audio_live);
    assert(event_counts[LINX_VOICE_ERROR] == 1 && audio_sends == 0 && event_counts[LINX_VOICE_FINISHED] == 0);
    shutdown_retained();

    for (unsigned operation = 0; operation < 3; ++operation) {
        reset(); prewarm(); clear_frames();
        linx_voice_request_t request = {.generation = 8};
        request.command = operation == 0 ? LINX_VOICE_WIFI_SCAN : operation == 1 ? LINX_VOICE_WIFI_SAVE : LINX_VOICE_WIFI_SETUP;
        if (operation == 1) {
            strcpy(request.wifi_network.ssid, "candidate-net"); request.wifi_network.security = LINX_WIFI_PERSONAL;
            strcpy(request.wifi_password, "candidate-secret");
        }
        if (operation == 2) portal_submissions = 1;
        assert(linx_device_request(&request, NULL)); execute_retained();
        assert(!retained && !wifi_live && !mock_ws.live && !audio_live && session_allocations == 2);
        assert(event_counts[operation == 0 ? LINX_VOICE_WIFI_NETWORKS : LINX_VOICE_WIFI_SAVED] == 1);
        assert(audio_reads == 0 && ws_destroys == 1 && wifi_deinits == 2);
    }
    /* 交接关闭失败先报告错误，不同时启动配网或第二个 Wi-Fi 所有者。 */
    reset(); prewarm(); clear_frames(); failure = "esp_wifi_stop";
    request_voice(LINX_VOICE_WIFI_SETUP, 8, cached_context); execute_retained();
    assert(!retained && portal_starts == 0 && session_allocations == 1 && event_counts[LINX_VOICE_ERROR] == 1);
}

static void test_idle_generation_and_partial_frames(void)
{
    /* 等待刚超时，UI 就入队新代号；空闲轮询不得误删同目标已就绪的连接。 */
    reset(); prewarm(); clear_frames(); next_context = cached_context;
    idle_queue_polls = 1; enqueue_after_timeout = true; execute_retained();
    assert(current(8) && retained && retained->generation == 8 && retained->hello);
    assert(event_counts[LINX_VOICE_READY] == 2 && http_cleanups == 1 && ws_connects == 1 && audio_reads == 0);
    shutdown_retained();

    /* 半条旧消息、continuation 和半个控制帧都不能跨入新的录音轮次。 */
    for (unsigned partial = 0; partial < 3; ++partial) {
        reset(); prewarm(); clear_frames();
        if (partial == 0)
            frames[frame_count++] = (frame_t){.opcode = 1, .fin = true, .payload = (int)strlen(tts_start), .data = tts_start, .size = 5};
        if (partial == 1)
            frames[frame_count++] = (frame_t){.opcode = 1, .fin = false, .payload = 5, .data = tts_start, .size = 5};
        if (partial == 2)
            frames[frame_count++] = (frame_t){.opcode = 9, .fin = true, .payload = 2, .data = "p", .size = 1};
        idle_poll();
        assert(retained->hello && audio_reads == 0 && audio_writes == 0);
        clear_frames(); add_wait(0); add_text(hello_opus); add_reply(1);
        request_voice(LINX_VOICE_START, 8, cached_context); execute_retained();
        assert(event_counts[LINX_VOICE_FINISHED] == 1 && event_counts[LINX_VOICE_ERROR] == 0);
        assert(http_cleanups == 2 && ws_connects == 2 && connect_calls == 1 && audio_writes == 1);
        shutdown_retained();
    }
}

static void test_audio_shutdown_barrier(void)
{
    reset(); prewarm(); clear_frames(); add_wait(0); add_reply(1);
    audio_sleep_failures = 3;
    request_voice(LINX_VOICE_START, 8, cached_context); execute_retained();
    assert(retained && retained->audio_awake && audio_live && !retained->hello && !retained->ws);
    assert(event_counts[LINX_VOICE_FINISHED] == 0 && event_counts[LINX_VOICE_ERROR] == 1 && wifi_running);
    /* 停服还不明确时，PREPARE 只能继续收尾，不能报 READY 或重新唤醒。 */
    clear_frames(); request_voice(LINX_VOICE_PREPARE, 9, cached_context); execute_retained();
    assert(!retained->audio_awake && !audio_live && event_counts[LINX_VOICE_READY] == 1);
    assert(event_counts[LINX_VOICE_ERROR] == 2 && http_cleanups == 1 && listen_starts == 1);
    clear_frames(); add_text(hello_opus);
    request_voice(LINX_VOICE_PREPARE, 10, cached_context); execute_retained();
    assert(retained->hello && event_counts[LINX_VOICE_READY] == 2 && audio_reads == 1);
    shutdown_retained();

    /* 持续停服失败时，配网进入唯一所有者的收尾屏障，不并发开启热点。 */
    reset(); prewarm(); clear_frames(); add_wait(0); add_reply(1); audio_sleep_failures = 5;
    request_voice(LINX_VOICE_START, 8, cached_context); execute_retained();
    assert(retained && retained->audio_awake && audio_live);
    request_voice(LINX_VOICE_WIFI_SETUP, 9, cached_context); execute_retained();
    assert(!retained && !pending_cleanup && !audio_live && !wifi_running && portal_starts == 0);
    assert(session_allocations == 1 && session_frees == 1 && delay_calls == 8);
    linx_voice_event_t event;
    assert(linx_device_poll(&event) && event.generation == 9 && event.state == LINX_VOICE_ERROR && event.error == LINX_VOICE_AUDIO_FAILED);

    /* 用户取消也需保留未确认停服状态，由空闲 owner 重试。 */
    reset(); prewarm(); clear_frames(); add_wait(0); add_wait(2); audio_sleep_failures = 2;
    request_voice(LINX_VOICE_START, 8, cached_context); execute_retained();
    assert(retained && retained->audio_awake && !retained->ws && audio_live && wifi_running);
    idle_queue_polls = 1; execute_retained();
    assert(!retained->audio_awake && !audio_live && event_counts[LINX_VOICE_READY] == 1);
    shutdown_retained();
}

static void test_warmup_handoff_all_phases(void)
{
    for (unsigned phase = 1; phase <= 4; ++phase) {
        for (unsigned starts = 0; starts < 2; ++starts) {
            reset(); next_context = cached_context;
            handoff_phase = phase; handoff_command = starts ? LINX_VOICE_START : LINX_VOICE_PREPARE;
            request_voice(LINX_VOICE_PREPARE, 7, cached_context);
            add_text(hello_opus);
            if (starts) add_reply(1);
            execute_retained();
            assert(retained && retained->generation == 8 && retained->handled_generation == 8);
            assert(connect_calls == 1 && http_cleanups == 1 && ws_connects == 1 && ws_destroys == 0);
            assert(session_allocations == 1 && session_frees == 0);
            assert(listen_starts == starts && event_counts[LINX_VOICE_FINISHED] == starts);
            assert(event_counts[LINX_VOICE_READY] == (starts ? 0U : 1U));
            assert(!audio_live && !codec_live);
            shutdown_retained();
        }
    }
    /* 预热中的普通退出继续完成连接，不能把页面取消当作硬释放。 */
    reset(); request_voice(LINX_VOICE_PREPARE, 7, cached_context);
    cancel_perform = true; add_text(hello_opus); execute_retained();
    assert(retained && retained->hello && !current(7) && listen_starts == 0 && audio_reads == 0);
    assert(ws_connects == 1 && http_cleanups == 1 && !retained->draining);
    clear_frames(); request_voice(LINX_VOICE_PREPARE, 8, cached_context); execute_retained();
    assert(retained->hello && ws_connects == 1 && http_cleanups == 1 && event_counts[LINX_VOICE_READY] == 1);
    shutdown_retained();
}

static void begin_cancelled_turn(unsigned stage)
{
    prewarm(); clear_frames(); add_wait(0);
    if (stage > 0) add_wait(1);
    if (stage > 1) { add_text(tts_start); add_binary("opus", 4); }
    add_wait(2);
    request_voice(LINX_VOICE_START, 8, cached_context); execute_retained();
    assert(retained && retained->hello && retained->draining && !retained->turn_active);
    assert(!audio_live && !codec_live && !atomic_load(&listening));
    assert(ws_connects == 1 && http_cleanups == 1 && connect_calls == 1 && ws_destroys == 0);
    assert(aborts == 1 && listen_stops == (stage ? 1U : 0U));
    assert(event_counts[LINX_VOICE_FINISHED] == 0 && event_counts[LINX_VOICE_ERROR] == 0);
}

static void test_soft_cancel_all_turn_phases(void)
{
    for (unsigned stage = 0; stage < 3; ++stage) {
        reset(); begin_cancelled_turn(stage);
        unsigned previous_reads = audio_reads, previous_writes = audio_writes;
        int64_t cancelled_deadline = retained->idle_deadline;
        clear_frames(); request_voice(LINX_VOICE_PREPARE, 9, cached_context); execute_retained();
        assert(event_counts[LINX_VOICE_DRAINING] == 1 && retained->idle_deadline == cancelled_deadline);
        /* 旧回复的字幕、语音头、二进制和真实 stop 全部静音排空。 */
        clear_frames(); add_text("{\"type\":\"stt\",\"text\":\"old\"}");
        add_text(tts_start); add_binary("opus", 4); add_text(tts_stop);
        idle_queue_polls = 4; execute_retained();
        assert(!retained->draining && retained->drain_stopped && retained->hello);
        assert(audio_reads == previous_reads && audio_writes == previous_writes && !audio_live);
        assert(retained->idle_deadline == cancelled_deadline && event_counts[LINX_VOICE_READY] == 2);
        clear_frames(); add_wait(0); add_reply(1);
        request_voice(LINX_VOICE_START, 10, cached_context); execute_retained();
        assert(event_counts[LINX_VOICE_FINISHED] == 1 && listen_starts == 2 && aborts == 1);
        assert(ws_connects == 1 && http_cleanups == 1 && connect_calls == 1);
        assert(audio_writes == previous_writes + 1U && audio_reads == previous_reads + 1U);
        shutdown_retained();
    }
    /* 空闲 READY 上取消没有进行中的轮次，无需 abort，立刻再进也直接复用。 */
    reset(); prewarm(); clear_frames(); cancel(); execute_retained();
    assert(retained->hello && !retained->draining && aborts == 0 && audio_reads == 0);
    request_voice(LINX_VOICE_START, 8, cached_context); add_wait(0); add_reply(1); execute_retained();
    assert(ws_connects == 1 && http_cleanups == 1 && event_counts[LINX_VOICE_FINISHED] == 1);
    shutdown_retained();
}

static void test_draining_start_and_cancel(void)
{
    reset(); begin_cancelled_turn(0); clear_frames();
    /* 新一条显式 START 等旧 stop，排空期间不采集/播放。 */
    add_text(tts_start); add_binary("opus", 4); add_text(tts_stop); add_reply(1);
    request_voice(LINX_VOICE_START, 9, cached_context); execute_retained();
    assert(event_counts[LINX_VOICE_DRAINING] == 1 && event_counts[LINX_VOICE_FINISHED] == 1);
    assert(listen_starts == 2 && listen_stops == 1 && aborts == 1 && audio_reads == 2 && audio_writes == 1);
    assert(ws_connects == 1 && http_cleanups == 1 && !retained->draining);
    shutdown_retained();

    /* 等待屏障时再次 BACK，之后收到旧 stop 也不能自动开启已撤销的新录音。 */
    reset(); begin_cancelled_turn(0); clear_frames(); add_wait(2);
    request_voice(LINX_VOICE_START, 9, cached_context); execute_retained();
    assert(retained->draining && !current(9) && listen_starts == 1 && aborts == 1 && listen_stops == 0);
    clear_frames(); add_text(tts_start); add_binary("opus", 4); add_text(tts_stop);
    idle_queue_polls = 3; execute_retained();
    assert(!retained->draining && listen_starts == 1 && audio_reads == 1 && audio_writes == 0);
    assert(event_counts[LINX_VOICE_FINISHED] == 0 && !audio_live && http_cleanups == 1 && ws_connects == 1);
    clear_frames(); request_voice(LINX_VOICE_PREPARE, 10, cached_context); execute_retained();
    assert(listen_starts == 1 && event_counts[LINX_VOICE_READY] == 2);
    clear_frames(); add_wait(0); add_reply(1); request_voice(LINX_VOICE_START, 11, cached_context); execute_retained();
    assert(listen_starts == 2 && event_counts[LINX_VOICE_FINISHED] == 1 && ws_connects == 1);
    shutdown_retained();

    /* 没有真实 stop，不能把一次 abort 当 ACK，也不能在超时前偷偷重建。 */
    reset(); begin_cancelled_turn(0); clear_frames();
    clock_us = retained->idle_deadline - 1000000;
    int64_t new_operation = clock_us;
    request_voice(LINX_VOICE_START, 9, cached_context); execute_retained();
    assert(retained && retained->hello && retained->draining && !retained->turn_active);
    assert(event_counts[LINX_VOICE_DRAINING] == 1 && event_counts[LINX_VOICE_ERROR] == 1);
    assert(retained->idle_deadline == new_operation + 30000000 && clock_us >= retained->idle_deadline);
    assert(ws_connects == 1 && http_cleanups == 1 && listen_starts == 1 && audio_reads == 1 && audio_writes == 0);
    linx_voice_event_t event; assert(linx_device_poll(&event) && event.error == LINX_VOICE_TIMEOUT);
    idle_poll();
    assert(!retained->ws && wifi_running && wifi_stops == 0);
    shutdown_retained();
}

static void test_cancel_fragment_barrier(void)
{
    for (unsigned partial = 0; partial < 3; ++partial) {
        reset(); prewarm(); clear_frames(); add_wait(0); add_wait(1);
        if (partial == 0)
            frames[frame_count++] = (frame_t){.opcode = 1, .fin = true, .payload = (int)strlen(tts_start), .data = tts_start, .size = 5, .action = 2};
        if (partial == 1)
            frames[frame_count++] = (frame_t){.opcode = 1, .fin = false, .payload = 5, .data = tts_start, .size = 5, .action = 2};
        if (partial == 2)
            frames[frame_count++] = (frame_t){.opcode = 9, .fin = true, .payload = 2, .data = "p", .size = 1, .action = 2};
        request_voice(LINX_VOICE_START, 8, cached_context); execute_retained();
        assert(retained->draining && audio_writes == 0 && aborts == 1 && listen_stops == 1);
        clear_frames();
        if (partial == 0)
            frames[frame_count++] = (frame_t){.opcode = 1, .fin = true, .payload = (int)strlen(tts_start), .data = tts_start + 5, .size = (int)strlen(tts_start) - 5};
        if (partial == 1)
            frames[frame_count++] = (frame_t){.opcode = 0, .fin = true, .payload = (int)strlen(tts_start) - 5, .data = tts_start + 5, .size = (int)strlen(tts_start) - 5};
        if (partial == 2)
            frames[frame_count++] = (frame_t){.opcode = 9, .fin = true, .payload = 2, .data = "i", .size = 1};
        add_binary("opus", 4); add_text(tts_stop); add_reply(1);
        request_voice(LINX_VOICE_START, 9, cached_context); execute_retained();
        assert(event_counts[LINX_VOICE_FINISHED] == 1 && event_counts[LINX_VOICE_ERROR] == 0);
        assert(audio_writes == 1 && listen_starts == 2 && ws_connects == 1 && http_cleanups == 1);
        assert(pongs == (partial == 2 ? 1U : 0U));
        shutdown_retained();
    }
}

static void test_rolling_lease_and_hard_release(void)
{
    reset(); prewarm();
    for (uint32_t generation = 8; generation <= 9; ++generation) {
        int64_t prior_deadline = retained->idle_deadline;
        clock_us = prior_deadline - 1000000;
        clear_frames(); add_wait(0); add_wait(4); add_reply(1);
        request_voice(LINX_VOICE_START, generation, cached_context); execute_retained();
        assert(retained->idle_deadline == clock_us + 30000000 && retained->idle_deadline > prior_deadline + 29000000);
        assert(ws_connects == 1 && http_cleanups == 1 && connect_calls == 1 && ws_destroys == 0);
        assert(event_counts[LINX_VOICE_FINISHED] == generation - 7U && !audio_live);
    }
    shutdown_retained();
    /* 排空中的新 START 也续期，跨过旧到期点收到真实 stop 后仍使用原连接。 */
    reset(); begin_cancelled_turn(0); clear_frames();
    int64_t prior_deadline = retained->idle_deadline;
    clock_us = prior_deadline - 1000000;
    add_wait(10); add_text(tts_stop); add_wait(4); add_reply(1);
    request_voice(LINX_VOICE_START, 9, cached_context); execute_retained();
    assert(event_counts[LINX_VOICE_FINISHED] == 1 && retained->idle_deadline == clock_us + 30000000);
    assert(ws_connects == 1 && http_cleanups == 1 && listen_starts == 2 && audio_writes == 1);
    shutdown_retained();

    /* 已软取消、页面代号 inactive 后，匹配代号的 RELEASE 仍必须生效。 */
    reset(); begin_cancelled_turn(0); clear_frames();
    request_voice(LINX_VOICE_RELEASE, 8, cached_context); execute_retained();
    assert(!retained->ws && !retained->draining && wifi_running && wifi_stops == 0);
    request_voice(LINX_VOICE_PREPARE, 9, cached_context); add_text(hello_opus); execute_retained();
    assert(retained->hello && ws_connects == 2 && http_cleanups == 2 && audio_reads == 1);
    shutdown_retained();
}

static bool has_protocol_log(const char *expected)
{
    for (unsigned i = 0; i < protocol_log_count; ++i)
        if (strstr(protocol_logs[i], expected)) return true;
    return false;
}

static void test_protocol_diagnostics(void)
{
    reset(); begin(); add_text(hello_opus); add_wait(1);
    add_text(tts_start); add_binary("opus", 4); add_text(tts_stop); execute();
    assert(protocol_log_count == 6);
    assert(has_protocol_log("direction=tx type=hello"));
    assert(has_protocol_log("direction=rx type=hello"));
    assert(has_protocol_log("direction=tx type=listen.start"));
    assert(has_protocol_log("direction=tx type=listen.stop"));
    assert(has_protocol_log("direction=rx type=tts.start"));
    assert(has_protocol_log("direction=rx type=tts.stop"));
    for (unsigned i = 0; i < protocol_log_count; ++i) {
        assert(strstr(protocol_logs[i], " bytes="));
        assert(strstr(protocol_logs[i], "ok=1 generation=7"));
    }
    assert(audio_sends == 1 && audio_writes == 1 && event_counts[LINX_VOICE_FINISHED] == 1);

    /* 日志只使用解析后的枚举，未知状态、私人正文和凭据不能成为日志字段。 */
    const char *incoming[] = {
        "{\"type\":\"stt\",\"text\":\"private-text\",\"token\":\"secret-token\",\"session_id\":\"session-1\"}",
        "{\"type\":\"private-text\",\"text\":\"private-text\"}",
        "{\"type\":\"tts\",\"state\":\"private-text\",\"text\":\"private-text\"}",
        "{\"type\":\"tts\",\"state\":false,\"text\":\"private-text\"}",
        "private-text",
    };
    const char *expected[] = {"stt", "unknown", "unknown", "invalid", "invalid"};
    for (unsigned i = 0; i < sizeof(incoming) / sizeof(incoming[0]); ++i) {
        reset();
        voice_session_t session = {.generation = 77, .hello = true, .turn_active = true};
        strcpy(session.session_id, "session-1");
        strcpy((char *)session.receiver.data, incoming[i]);
        session.receiver.used = strlen(incoming[i]);
        linx_voice_error_t result = message(&session);
        bool valid = i < 3;
        assert(result == (valid ? LINX_VOICE_OK : LINX_VOICE_PROTOCOL_FAILED));
        assert(protocol_log_count == 1);
        char line[128];
        snprintf(line, sizeof(line), "direction=rx type=%s bytes=%zu parse_ok=%d generation=77",
                 expected[i], strlen(incoming[i]), valid);
        assert(strstr(protocol_logs[0], line));
    }

    reset(); begin(); failure = "esp_transport_ws_send_raw";
    execute(); expect_error(LINX_VOICE_NETWORK_FAILED);
    assert(protocol_log_count == 1 && strstr(protocol_logs[0], "direction=tx type=hello"));
    assert(strstr(protocol_logs[0], "ok=0 generation=7"));
    assert(diagnostic_count == 1 && strstr(diagnostics[0], "operation=send phase=hello"));

    reset(); begin_cancelled_turn(0);
    assert(has_protocol_log("direction=tx type=abort"));
    shutdown_retained();
}

static void test_transport_diagnostics(void)
{
    static const uint8_t close_with_private_reason[] = {0x03, 0xf3, 'd', 'i', 'a', 'g', 'n', 'o', 's', 't', 'i', 'c', '-', 'p', 'r', 'i', 'v', 'a', 't', 'e'};
    const char *operations[] = {"esp_transport_poll_read", "esp_transport_read", "esp_transport_ws_send_raw", NULL};
    const char *log_operations[] = {"poll_read", "read", "send", "close"};
    for (unsigned phase = 1; phase <= 2; ++phase) {
        for (unsigned operation = 0; operation < 4; ++operation) {
            reset(); begin();
            add_text(hello_opus); add_wait(1);
            if (phase == 2) add_text(tts_start);
            if (operation == 3)
                frames[frame_count++] = (frame_t){.opcode = 8, .fin = true, .payload = sizeof(close_with_private_reason),
                    .data = close_with_private_reason, .size = sizeof(close_with_private_reason)};
            else frames[frame_count++] = (frame_t){.opcode = 9, .fin = true, .payload = 1, .data = "p", .size = 1};
            diagnostic_fault_stage = phase; failure = operations[operation];
            cached_socket_errno = 104;
            cached_tls_error = (struct test_tls_error){.esp_code = 0x8018, .tls_code = -0x1234, .flags = 7};
            execute(); expect_error(LINX_VOICE_NETWORK_FAILED);
            assert(diagnostic_count == 1 && tls_error_reads == 1 && transport_errno_reads == 1);
            char expected[80];
            snprintf(expected, sizeof(expected), "operation=%s phase=%s", log_operations[operation], phase == 1 ? "waiting" : "playing");
            assert(strstr(diagnostics[0], expected));
            assert(strstr(diagnostics[0], "errno=104 esp=32792 tls=-4660 flags=7"));
            assert(strstr(diagnostics[0], operation == 3 ? "close=1011" : "close=-1"));
            assert(listen_starts == 1 && listen_stops == 1 && audio_reads == 1 && audio_sends == 1);
            assert(http_cleanups == 1 && ws_connects == 1 && ws_destroys == 1 && audio_sleeps == 1);
            assert(cached_socket_errno == 0 && cached_tls_error.esp_code == 0);
        }
    }
    /* 0字节超时与没有完整close code分别留数字证据，继续沿用当前失败策略。 */
    for (unsigned empty = 0; empty < 3; ++empty) {
        reset(); begin(); add_text(hello_opus); add_wait(1);
        if (empty == 0) frames[frame_count++] = (frame_t){.opcode = 1, .payload = 0, .action = 11};
        if (empty == 1) frames[frame_count++] = (frame_t){.opcode = 1, .fin = true, .payload = 4};
        if (empty == 2) frames[frame_count++] = (frame_t){.opcode = 8, .fin = true, .payload = 1, .data = "x", .size = 1};
        missing_error_handle = true; execute(); expect_error(LINX_VOICE_NETWORK_FAILED);
        assert(diagnostic_count == 1 && tls_error_reads == 0 && transport_errno_reads == 1);
        assert(strstr(diagnostics[0], "phase=waiting") && strstr(diagnostics[0], "close=-1"));
        assert(listen_stops == 1 && ws_connects == 1 && audio_sends == 1);
    }
}

static void release_network_owner(void)
{
    linx_device_network_release();
    shutdown_retained();
    assert(!retained && !pending_cleanup);
}

static void test_network_only_owner(void)
{
    reset(); assert(!linx_device_network_poll(NULL));
    failure = "xTaskCreate"; assert(!linx_device_network_acquire()); assert(!atomic_load(&network_wanted));
    failure = NULL; assert(linx_device_network_acquire());
    assert(linx_device_network_acquire() && task_calls == 2);
    linx_device_network_state_t state;
    assert(linx_device_network_poll(&state) && !state.ready && !state.provisioning);
    assert(!atomic_load(&active) && !atomic_load(&listening) && !commands->ready);
    network_tick(); assert(retained && retained->network_only);
    assert(linx_device_network_poll(&state) && state.ready && state.error == LINX_VOICE_OK);
    assert(!strcmp(state.mac_address, retained->device_id));
    assert(connect_calls == 1 && !http_cleanups && !ws_connects && !audio_reads);
    voice_session_t *owner = retained;
    prewarm(); assert(retained == owner && connect_calls == 1);
    network_tick(); assert(atomic_load(&network_ready));
    clock_us = owner->idle_deadline; idle_poll(); network_tick();
    assert(!mock_ws.live && wifi_running && retained == owner && atomic_load(&network_ready));
    request_voice(LINX_VOICE_RELEASE, 7, cached_context); execute_retained();
    assert(wifi_running && retained == owner && atomic_load(&network_ready));
    release_network_owner();
    reset(); assert(linx_device_network_acquire()); network_tick();
    linx_device_network_release(); network_tick(); assert(!retained && !wifi_running);
    reset(); assert(linx_device_network_acquire()); atomic_store(&network_provisioning, true); network_tick();
    assert(!retained && !session_allocations); atomic_store(&network_provisioning, false);
    network_tick(); auto_network_ack = true;
    uint32_t old_epoch = atomic_load(&network_epoch);
    wifi_event(retained, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, NULL);
    assert(!atomic_load(&network_ready) && atomic_load(&network_epoch) == old_epoch + 1);
    linx_device_network_closed(old_epoch); assert(atomic_load(&network_close_ack) == old_epoch);
    network_tick(); assert(atomic_load(&network_ready) && session_allocations == 2 && connect_calls == 2);
    release_network_owner();
}

static void test_network_failures_and_provisioning(void)
{
    reset(); assert(linx_device_network_acquire()); session_oom = true; network_tick();
    assert(atomic_load(&network_error) == LINX_VOICE_NO_MEMORY && network_retry_at > clock_us);
    session_oom = false; network_tick(); assert(!retained);
    clock_us = network_retry_at; configured = false; network_tick();
    assert(!retained && atomic_load(&network_error) == LINX_VOICE_NOT_CONFIGURED);
    clock_us = network_retry_at; configured = true; strcpy((char *)flash_config.sta.ssid, "existing-net"); network_tick(); assert(atomic_load(&network_ready));
    voice_session_t *owner = retained;
    request_voice(LINX_VOICE_WIFI_SETUP, 7, cached_context); execute_retained();
    assert(retained == owner && wifi_running && !portal_starts && !wifi_stops);
    expect_error(LINX_VOICE_TIMEOUT); assert(!atomic_load(&network_provisioning));
    assert(!atomic_load(&network_ready) && atomic_load(&network_error) == LINX_VOICE_TIMEOUT);
    auto_network_ack = true; linx_device_network_closed(atomic_load(&network_epoch));
    request_voice(LINX_VOICE_WIFI_SETUP, 7, cached_context); portal_submissions = 1; execute_retained();
    assert(portal_starts == 1 && portal_statuses[LINX_WIFI_PORTAL_SAVED] == 1 && !atomic_load(&network_provisioning));
    network_tick(); assert(atomic_load(&network_ready)); release_network_owner();
    reset(); assert(linx_device_network_acquire()); network_tick();
    auto_network_ack = true; request_voice(LINX_VOICE_WIFI_SETUP, 7, cached_context); portal_cancel = true; portal_stop_failures = 1;
    execute_retained(); assert(!pending_cleanup && !atomic_load(&network_provisioning));
    network_tick(); assert(atomic_load(&network_ready)); release_network_owner();
    reset(); assert(linx_device_network_acquire()); wall_time = 0; time_ready_delay = 1; network_tick();
    assert(atomic_load(&network_ready) && sntp_stops == 1); release_network_owner();
    reset(); assert(linx_device_network_acquire()); wall_time = 0; network_tick();
    assert(!retained && atomic_load(&network_error) == LINX_VOICE_TIME_FAILED); linx_device_network_release();
    reset(); assert(linx_device_network_acquire()); wall_time = 0; release_network_on_delay = true; network_tick();
    assert(!atomic_load(&network_ready) && !atomic_load(&network_wanted)); release_network_owner();
    reset(); assert(linx_device_network_acquire()); network_tick();
    /* 试连 IP 只更新试连状态，不能对 Gateway 开放 ready。 */
    network_invalidate(); retained->trial = retained->trial_accepting = true;
    atomic_store(&retained->trial_associated, true);
    wifi_event(retained, IP_EVENT, IP_EVENT_STA_GOT_IP, NULL);
    assert(!atomic_load(&network_ready));
    auto_network_ack = true; release_network_owner();
}

static void gateway_voice(linx_voice_command_t command, uint32_t generation, const char *target, const char *request_id)
{
    linx_voice_request_t request = {.command = command, .generation = generation, .context = cached_context};
    strcpy(request.gateway_agent_session_id, target);
    strcpy(request.gateway_request_id, request_id);
    assert(linx_device_request(&request, NULL));
}

static void pcm_reply(void)
{
    static const uint8_t samples[640] = {1, 0, 2, 0};
    add_text(tts_start); add_binary(samples, sizeof(samples)); add_text(tts_stop);
}

static void test_gateway_same_agent_voice(void)
{
    reset(); assert(linx_device_network_acquire()); network_tick();
    voice_session_t *owner = retained;
    uint32_t epoch = atomic_load(&network_epoch);
    gateway_voice(LINX_VOICE_PREPARE, 7, "real-session-a", ""); add_text(hello_pcm); execute_retained();
    assert(retained == owner && retained->hello && !audio_live && !listen_starts && !audio_reads);
    for (uint32_t generation = 8; generation <= 9; ++generation) {
        clock_us = retained->idle_deadline - 1000000LL;
        clear_frames(); add_wait(0); add_wait(1);
        add_text("{\"type\":\"stt\",\"text\":\"今天天气如何\"}"); pcm_reply();
        if (generation == 8) gateway_voice(LINX_VOICE_START, generation, "real-session-a", "round-8");
        else {
            linx_voice_request_t request = {.command = LINX_VOICE_START, .generation = generation,
                                            .context = {.scene_id = 99, .task_id = 777, .source_turn_id = 888}};
            strcpy(request.gateway_agent_session_id, "real-session-a");
            strcpy(request.gateway_request_id, "round-9");
            /* Gateway真实目标相同，展示来源变化不应当更换语音连接。 */
            assert(linx_device_request(&request, NULL));
        }
        execute_retained();
        assert(retained == owner && retained->hello && retained->ws && !retained->turn_active);
        assert(!strcmp(recorded_request_id, generation == 8 ? "round-8" : "round-9"));
        assert(event_counts[LINX_VOICE_FINISHED] == generation - 7U && audio_writes == generation - 7U);
        assert(listen_starts == generation - 7U && listen_stops == generation - 7U && audio_reads >= generation - 7U);
        assert(ws_connects == 1 && ws_closes == 0 && http_cleanups == 1 && connect_calls == 1 && !opus_opens);
        assert(retained->idle_deadline == clock_us + 30000000LL);
        assert(atomic_load(&network_ready) && atomic_load(&network_epoch) == epoch);
    }
    clear_frames(); gateway_voice(LINX_VOICE_PREPARE, 10, "real-session-b", "");
    add_text(hello_pcm); execute_retained();
    assert(!strcmp(retained->gateway_agent_session_id, "real-session-b") && ws_connects == 2 && ws_closes == 1);
    assert(connect_calls == 1 && atomic_load(&network_epoch) == epoch);
    auto_network_ack = true; release_network_owner();

    /* STT是旁观事件；没有 Agent TTS 时超时，不能据此“完成”或新建回执连接。 */
    reset(); gateway_voice(LINX_VOICE_START, 7, "real-session-a", "round-7");
    add_text(hello_pcm); add_wait(1); add_text("{\"type\":\"stt\",\"text\":\"原话\"}"); add_wait(4);
    execute_retained(); expect_error(LINX_VOICE_TIMEOUT);
    assert(!event_counts[LINX_VOICE_FINISHED] && !audio_writes && ws_connects == 1 && !mock_ws.live);
    shutdown_retained();

    /* Gateway目标同样保留取消屏障；新请求号不能跳过旧tts.stop。 */
    reset(); gateway_voice(LINX_VOICE_PREPARE, 7, "real-session-a", ""); add_text(hello_pcm); execute_retained();
    clear_frames(); add_wait(0); add_wait(2);
    gateway_voice(LINX_VOICE_START, 8, "real-session-a", "cancelled-round"); execute_retained();
    assert(retained->draining && retained->ws && !audio_live && aborts == 1);
    clear_frames(); add_text(tts_stop); add_wait(0); add_wait(1); pcm_reply();
    gateway_voice(LINX_VOICE_START, 9, "real-session-a", "next-round"); execute_retained();
    assert(event_counts[LINX_VOICE_FINISHED] == 1 && listen_starts == 2 && ws_connects == 1 && !retained->draining);
    assert(!strcmp(recorded_request_id, "next-round") && audio_writes == 1);
    shutdown_retained();
}

static void test_gateway_voice_request_validation(void)
{
    reset();
    linx_voice_request_t request = {.command = LINX_VOICE_START, .generation = 7};
    strcpy(request.gateway_request_id, "round"); assert(!linx_device_request(&request, NULL));
    strcpy(request.gateway_agent_session_id, "target"); request.gateway_request_id[0] = 0;
    assert(!linx_device_request(&request, NULL));
    strcpy(request.gateway_request_id, "round");
    memset(request.gateway_agent_session_id, 'x', sizeof(request.gateway_agent_session_id));
    assert(!linx_device_request(&request, NULL));
    strcpy(request.gateway_agent_session_id, "target");
    memset(request.gateway_request_id, 'x', sizeof(request.gateway_request_id));
    assert(!linx_device_request(&request, NULL));
    strcpy(request.gateway_request_id, "bad\n"); assert(!linx_device_request(&request, NULL));
    strcpy(request.gateway_request_id, "round"); strcpy(request.gateway_agent_session_id, "bad\n");
    assert(!linx_device_request(&request, NULL) && !commands && !events);
    reset(); gateway_voice(LINX_VOICE_PREPARE, 7, "target", ""); cancel(); execute();
    reset(); gateway_voice(LINX_VOICE_START, 7, "target", "round");
    add_text(hello_opus); add_wait(1); add_text(tts_start); add_binary("opus", 4); add_text(tts_stop); execute();
    assert(opus_opens == 1 && opus_decodes == 1 && opus_closes == 1 && audio_writes == 1);
    assert(event_counts[LINX_VOICE_FINISHED] == 1 && !strcmp(recorded_request_id, "round"));
}

int main(void)
{
    test_gateway_same_agent_voice();
    test_gateway_voice_request_validation();
    test_network_only_owner();
    test_network_failures_and_provisioning();
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
    test_prepare_and_reuse();
    test_idle_expiry_and_rebuild();
    test_target_changes();
    test_invalid_idle_connection();
    test_prepare_start_and_cancel_races();
    test_retained_failures_and_provisioning();
    test_idle_generation_and_partial_frames();
    test_audio_shutdown_barrier();
    test_warmup_handoff_all_phases();
    test_soft_cancel_all_turn_phases();
    test_draining_start_and_cancel();
    test_cancel_fragment_barrier();
    test_rolling_lease_and_hard_release();
    test_protocol_diagnostics();
    test_transport_diagnostics();
    reset();
    puts("灵矽设备适配器：生命周期、流式音频、取消及失败清理测试通过");
    return 0;
}
