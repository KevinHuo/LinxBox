#include "linx_gateway_transport.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_transport_ssl.h"
#include "esp_transport_tcp.h"
#include "esp_transport_ws.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "linx_device.h"
#include "linx_protocol.h"

#ifndef CONFIG_LINX_GATEWAY_ENABLE
#define CONFIG_LINX_GATEWAY_ENABLE 0
#endif
#ifndef CONFIG_LINX_GATEWAY_URL
#define CONFIG_LINX_GATEWAY_URL ""
#endif
#ifndef CONFIG_LINX_GATEWAY_TOKEN
#define CONFIG_LINX_GATEWAY_TOKEN ""
#endif
#ifndef CONFIG_LINX_GATEWAY_ALLOW_WS
#define CONFIG_LINX_GATEWAY_ALLOW_WS 0
#endif

#define TICK_MS 20
#define IO_TIMEOUT_MS 1000
#define CONNECT_TIMEOUT_MS 5000
#define BACKOFF_MAX_MS 30000U
#define RX_CHUNK_SIZE 1024U
#define INFRASTRUCTURE_FREE_BYTES (32U * 1024U)
#define INFRASTRUCTURE_BLOCK_BYTES (20U * 1024U)

/* 两个单槽都拒绝覆盖。FRAME 借出期间停止读下一帧，不会丢 response。 */
static portMUX_TYPE mailbox_lock = portMUX_INITIALIZER_UNLOCKED;
static atomic_bool started;
static atomic_bool wanted;
static atomic_bool retry_requested;
static atomic_bool connected;
static atomic_uint public_generation;
static bool event_pending;
static bool frame_ready;
static bool frame_borrowed;
static linx_gateway_transport_event_t mailbox;
static char *outgoing;
static size_t outgoing_size;
static uint32_t outgoing_generation;
static esp_transport_handle_t base_transport;
static esp_transport_handle_t websocket;
static uint32_t connection_generation;
static uint32_t connection_epoch;
static char connection_mac[18];
static bool disconnect_pending;
static bool network_acquired;
static bool config_reported;
static int64_t reconnect_at;
static unsigned backoff_ms = 1000U;
static linx_voice_error_t last_network_error;
static char *incoming;
static size_t incoming_capacity;
static size_t incoming_size;
static size_t frame_remaining;
static unsigned frame_opcode;
static bool frame_fin;
static bool fragmented;
static bool frame_active;
static uint8_t control[125];
static size_t control_used;
static int64_t frame_deadline;

static bool mailbox_busy(void)
{
    taskENTER_CRITICAL(&mailbox_lock);
    bool busy = event_pending;
    taskEXIT_CRITICAL(&mailbox_lock);
    return busy;
}

static void post(linx_gateway_transport_event_type_t type, int error)
{
    taskENTER_CRITICAL(&mailbox_lock);
    mailbox = (linx_gateway_transport_event_t){.type = type, .generation = connection_generation,
        .error = error};
    memcpy(mailbox.mac_address, connection_mac, sizeof(mailbox.mac_address));
    if (type == LINX_GATEWAY_TRANSPORT_FRAME) {
        mailbox.data = incoming;
        mailbox.size = incoming_size;
        frame_ready = true;
    }
    event_pending = true;
    taskEXIT_CRITICAL(&mailbox_lock);
}

static void receive_reset(void)
{
    incoming_size = frame_remaining = control_used = 0;
    fragmented = frame_active = false;
    frame_deadline = 0;
}

static void drop_outgoing(void)
{
    taskENTER_CRITICAL(&mailbox_lock);
    char *buffer = outgoing;
    outgoing = NULL;
    outgoing_size = 0;
    taskEXIT_CRITICAL(&mailbox_lock);
    free(buffer);
}

static void close_connection(void)
{
    atomic_store(&connected, false);
    if (websocket) {
        esp_transport_close(websocket);
        esp_transport_destroy(websocket);
        websocket = NULL;
        disconnect_pending = true;
    }
    if (base_transport) esp_transport_destroy(base_transport);
    base_transport = NULL;
    drop_outgoing();
    taskENTER_CRITICAL(&mailbox_lock);
    bool held = frame_ready;
    taskEXIT_CRITICAL(&mailbox_lock);
    if (!held) {
        free(incoming);
        incoming = NULL;
        incoming_capacity = 0;
        receive_reset();
    }
}

static void fail(int error)
{
    close_connection();
    reconnect_at = esp_timer_get_time() + backoff_ms * 1000LL;
    if (backoff_ms < BACKOFF_MAX_MS) {
        backoff_ms *= 2U;
        if (backoff_ms > BACKOFF_MAX_MS) backoff_ms = BACKOFF_MAX_MS;
    }
    post(LINX_GATEWAY_TRANSPORT_ERROR, error);
}

static bool config_valid(void)
{
    if (!CONFIG_LINX_GATEWAY_ENABLE || !CONFIG_LINX_GATEWAY_URL[0] || !CONFIG_LINX_GATEWAY_TOKEN[0]) return false;
    size_t size = strlen(CONFIG_LINX_GATEWAY_TOKEN);
    if (size > 1024U) return false;
    for (size_t i = 0; i < size; ++i) {
        unsigned char byte = (unsigned char)CONFIG_LINX_GATEWAY_TOKEN[i];
        if (byte < 0x21U || byte > 0x7eU) return false;
    }
    linx_endpoint_t endpoint;
    return linx_parse_gateway_endpoint(CONFIG_LINX_GATEWAY_URL, CONFIG_LINX_GATEWAY_ALLOW_WS, &endpoint);
}

static bool connect_gateway(void)
{
    linx_endpoint_t endpoint;
    if (!linx_parse_gateway_endpoint(CONFIG_LINX_GATEWAY_URL, CONFIG_LINX_GATEWAY_ALLOW_WS, &endpoint)) return false;
    base_transport = endpoint.tls ? esp_transport_ssl_init() : esp_transport_tcp_init();
    if (!base_transport) return false;
    /* 使用系统根证书并保留 SDK 默认的主机名、有效期检查。 */
    if (endpoint.tls) esp_transport_ssl_crt_bundle_attach(base_transport, esp_crt_bundle_attach);
    websocket = esp_transport_ws_init(base_transport);
    if (!websocket) return false;
    char authorization[1032];
    int count = snprintf(authorization, sizeof(authorization), "Bearer %s", CONFIG_LINX_GATEWAY_TOKEN);
    if (count < 0 || (size_t)count >= sizeof(authorization)) return false;
    esp_transport_ws_config_t config = {.ws_path = endpoint.path, .user_agent = "LinxBox-Gateway/1.0",
        .auth = authorization, .propagate_control_frames = true};
    esp_err_t result = esp_transport_ws_set_config(websocket, &config);
    memset(authorization, 0, sizeof(authorization));
    if (result != ESP_OK) return false;
    if (esp_transport_connect(websocket, endpoint.host, endpoint.port, CONNECT_TIMEOUT_MS) < 0) return false;
    ++connection_generation;
    if (!connection_generation) ++connection_generation;
    atomic_store(&public_generation, connection_generation);
    atomic_store(&connected, true);
    backoff_ms = 1000U;
    receive_reset();
    return true;
}

static bool send_frame(unsigned opcode, const char *data, size_t size)
{
    int sent = esp_transport_ws_send_raw(websocket, (ws_transport_opcodes_t)(opcode | WS_TRANSPORT_OPCODES_FIN),
        data, (int)size, IO_TIMEOUT_MS);
    /* 短写的帧不可重新发送，否则可能执行两次。 */
    return sent >= 0 && (size_t)sent == size;
}

/* 展示收发缓存不能用尽联网余量；realloc 按新旧块同时存在的最坏情况预算。 */
static bool application_memory_available(size_t size)
{
    uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    size_t available = heap_caps_get_free_size(caps);
    size_t largest = heap_caps_get_largest_free_block(caps);
    return available >= INFRASTRUCTURE_FREE_BYTES && size <= available - INFRASTRUCTURE_FREE_BYTES &&
           largest >= INFRASTRUCTURE_BLOCK_BYTES && size <= largest - INFRASTRUCTURE_BLOCK_BYTES;
}

static bool reserve_incoming(size_t size)
{
    if (size > LINX_GATEWAY_FRAME_CAPACITY) return false;
    if (size + 1U <= incoming_capacity) return true;
    if (!application_memory_available(size + 1U)) return false;
    char *buffer = realloc(incoming, size + 1U);
    if (!buffer) return false;
    incoming = buffer;
    incoming_capacity = size + 1U;
    return true;
}

static bool receive_chunk(unsigned opcode, bool fin, size_t payload, const char *chunk, size_t size)
{
    if (!frame_active) {
        if (opcode == WS_TRANSPORT_OPCODES_PING || opcode == WS_TRANSPORT_OPCODES_PONG ||
            opcode == WS_TRANSPORT_OPCODES_CLOSE) {
            if (!fin || payload > sizeof(control) || (opcode == WS_TRANSPORT_OPCODES_CLOSE && payload == 1U)) return false;
            control_used = 0;
        } else {
            if (opcode == WS_TRANSPORT_OPCODES_TEXT) {
                if (fragmented) return false;
                incoming_size = 0;
            } else if (opcode != WS_TRANSPORT_OPCODES_CONT || !fragmented) return false;
            if (payload > LINX_GATEWAY_FRAME_CAPACITY - incoming_size ||
                !reserve_incoming(incoming_size + payload)) return false;
        }
        frame_opcode = opcode;
        frame_fin = fin;
        frame_remaining = payload;
        frame_active = true;
        if (!frame_deadline) frame_deadline = esp_timer_get_time() + IO_TIMEOUT_MS * 1000LL;
    } else if (opcode != frame_opcode || fin != frame_fin) return false;
    if (size > frame_remaining) return false;
    bool control_frame = frame_opcode >= WS_TRANSPORT_OPCODES_CLOSE;
    if (control_frame) {
        if (size) memcpy(control + control_used, chunk, size);
        control_used += size;
    } else {
        if (size) memcpy(incoming + incoming_size, chunk, size);
        incoming_size += size;
    }
    frame_remaining -= size;
    if (frame_remaining) return true;
    frame_active = false;
    if (control_frame) {
        if (frame_opcode == WS_TRANSPORT_OPCODES_CLOSE) return false;
        if (frame_opcode == WS_TRANSPORT_OPCODES_PING &&
            !send_frame(WS_TRANSPORT_OPCODES_PONG, (const char *)control, control_used)) return false;
        if (!fragmented) frame_deadline = 0;
    } else if (frame_fin) {
        incoming[incoming_size] = 0;
        fragmented = false;
        frame_deadline = 0;
        post(LINX_GATEWAY_TRANSPORT_FRAME, 0);
    } else fragmented = true;
    return true;
}

static bool read_gateway(void)
{
    if (frame_deadline && esp_timer_get_time() >= frame_deadline) return false;
    int ready = esp_transport_poll_read(websocket, 0);
    if (ready <= 0) return ready == 0;
    char chunk[RX_CHUNK_SIZE];
    int size = esp_transport_read(websocket, chunk, sizeof(chunk), IO_TIMEOUT_MS);
    unsigned opcode = (unsigned)esp_transport_ws_get_read_opcode(websocket);
    int payload = esp_transport_ws_get_read_payload_len(websocket);
    if (size < 0 || payload < 0 || opcode == WS_TRANSPORT_OPCODES_NONE) return false;
    return receive_chunk(opcode, esp_transport_ws_get_fin_flag(websocket), (size_t)payload, chunk, (size_t)size);
}

/* 单步供工作任务和 SDK 替身测试共用；每次最多读写一块。 */
static void transport_tick(void)
{
    linx_device_network_state_t network;
    linx_device_network_poll(&network);
    bool enabled = atomic_load(&wanted);
    if (atomic_exchange(&retry_requested, false)) {
        reconnect_at = 0;
        config_reported = false;
        last_network_error = LINX_VOICE_OK;
    }
    if (!enabled || !network.ready || network.provisioning || (websocket && network.epoch != connection_epoch)) {
        close_connection();
        /* 借出的旧消息归还并释放后才交出内存，语音 TLS 不与它争用。 */
        if (!incoming) linx_device_network_closed(network.epoch);
    }
    if (!enabled && network_acquired) {
        linx_device_network_release();
        network_acquired = false;
    }
    if (mailbox_busy()) return;
    if (disconnect_pending) {
        disconnect_pending = false;
        post(LINX_GATEWAY_TRANSPORT_DISCONNECTED, 0);
        return;
    }
    taskENTER_CRITICAL(&mailbox_lock);
    bool held = frame_ready;
    taskEXIT_CRITICAL(&mailbox_lock);
    if (held) return;
    if (!enabled) {
        free(incoming);
        incoming = NULL;
        incoming_capacity = 0;
        receive_reset();
        return;
    }
    if (!config_valid()) {
        if (!config_reported) post(LINX_GATEWAY_TRANSPORT_NOT_CONFIGURED, 0);
        config_reported = true;
        return;
    }
    if (!network_acquired) {
        if (!linx_device_network_acquire()) { fail(LINX_VOICE_NO_MEMORY); return; }
        network_acquired = true;
        return;
    }
    if (!network.ready || network.provisioning) {
        if (!network.provisioning && network.error != LINX_VOICE_OK && network.error != last_network_error) {
            last_network_error = network.error;
            post(LINX_GATEWAY_TRANSPORT_ERROR, network.error);
        }
        return;
    }
    last_network_error = LINX_VOICE_OK;
    if (!websocket) {
        if (esp_timer_get_time() < reconnect_at) return;
        connection_epoch = network.epoch;
        memcpy(connection_mac, network.mac_address, sizeof(connection_mac));
        if (!connect_gateway()) { fail(LINX_VOICE_NETWORK_FAILED); return; }
        post(LINX_GATEWAY_TRANSPORT_CONNECTED, 0);
        return;
    }
    taskENTER_CRITICAL(&mailbox_lock);
    size_t send_size = outgoing_size;
    const char *send_data = outgoing;
    uint32_t send_generation = outgoing_generation;
    taskEXIT_CRITICAL(&mailbox_lock);
    if (send_size) {
        bool sent = send_generation == connection_generation && send_frame(WS_TRANSPORT_OPCODES_TEXT, send_data, send_size);
        drop_outgoing();
        if (!sent) { fail(LINX_VOICE_NETWORK_FAILED); return; }
    }
    if (!read_gateway()) fail(LINX_VOICE_PROTOCOL_FAILED);
}

static void transport_worker(void *user)
{
    (void)user;
    for (;;) {
        transport_tick();
        taskENTER_CRITICAL(&mailbox_lock);
        bool done = !atomic_load(&wanted) && !event_pending && !frame_ready && !network_acquired;
        if (done) atomic_store(&started, false);
        taskEXIT_CRITICAL(&mailbox_lock);
        if (done) { vTaskDelete(NULL); return; }
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }
}

bool linx_gateway_transport_init(void)
{
    taskENTER_CRITICAL(&mailbox_lock);
    atomic_store(&wanted, true);
    atomic_store(&retry_requested, true);
    bool existing = atomic_load(&started);
    atomic_store(&started, true);
    taskEXIT_CRITICAL(&mailbox_lock);
    if (existing) return true;
    if (xTaskCreate(transport_worker, "linx_gateway", 6144, NULL, 4, NULL) != pdPASS) {
        atomic_store(&wanted, false);
        atomic_store(&started, false);
        return false;
    }
    return true;
}

bool linx_gateway_transport_poll(linx_gateway_transport_event_t *event)
{
    if (!event) return false;
    taskENTER_CRITICAL(&mailbox_lock);
    bool available = event_pending;
    if (available) {
        *event = mailbox;
        event_pending = false;
        if (event->type == LINX_GATEWAY_TRANSPORT_FRAME) frame_borrowed = true;
    }
    taskEXIT_CRITICAL(&mailbox_lock);
    return available;
}

void linx_gateway_transport_release(void)
{
    taskENTER_CRITICAL(&mailbox_lock);
    if (frame_borrowed) frame_ready = frame_borrowed = false;
    taskEXIT_CRITICAL(&mailbox_lock);
}

bool linx_gateway_transport_send(uint32_t generation, const char *json, size_t size, void *user)
{
    (void)user;
    if (!json || !size || size > LINX_GATEWAY_SEND_CAPACITY) return false;
    taskENTER_CRITICAL(&mailbox_lock);
    bool available = !outgoing && atomic_load(&wanted) && atomic_load(&connected) &&
        generation == atomic_load(&public_generation);
    taskEXIT_CRITICAL(&mailbox_lock);
    if (!available) return false;
    if (!application_memory_available(size)) return false;
    char *buffer = malloc(size);
    if (!buffer) return false;
    memcpy(buffer, json, size);
    /* Allocation runs outside the lock; a disconnect or another sender may win. */
    taskENTER_CRITICAL(&mailbox_lock);
    bool accepted = !outgoing && atomic_load(&wanted) && atomic_load(&connected) &&
        generation == atomic_load(&public_generation);
    if (accepted) {
        outgoing = buffer;
        outgoing_generation = generation;
        outgoing_size = size;
    }
    taskEXIT_CRITICAL(&mailbox_lock);
    if (!accepted) free(buffer);
    return accepted;
}

void linx_gateway_transport_retry(void)
{
    atomic_store(&retry_requested, true);
}

void linx_gateway_transport_deinit(void)
{
    atomic_store(&wanted, false);
}
