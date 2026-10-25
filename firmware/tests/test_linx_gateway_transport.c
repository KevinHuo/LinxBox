#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define LINX_GATEWAY_TRANSPORT_TEST 1
#include "sdk_stubs.h"
static void *mock_realloc(void *pointer, size_t size);
static void *mock_malloc(size_t size);
static void mock_free(void *pointer);
#define malloc mock_malloc
#define free mock_free
#define realloc mock_realloc
#include "../main/linx_gateway_transport.c"
#undef realloc
#undef malloc
#undef free

bool test_gateway_enable = true;
bool test_gateway_allow_ws;
const char *test_gateway_url = "wss://gateway.example.org/device";
const char *test_gateway_token = "device-token";
struct test_transport {bool live;};
static struct test_transport mock_ssl, mock_tcp, mock_ws;
static unsigned ssl_inits, tcp_inits, certificate_attaches;
static const char *expected_host, *expected_path;
static int expected_port;
static linx_device_network_state_t network;
static int64_t now;
static const char *failure;
static unsigned destroys, closes, connects, sends, pongs, acquisitions, releases, acknowledgements, tasks;
static uint32_t acknowledged;
static unsigned task_deletes;
static char sent_data[LINX_GATEWAY_SEND_CAPACITY + 1U];
static void (*task_function)(void *);
static jmp_buf worker_end;
static bool alloc_failed, send_alloc_failed, deinit_on_send;
static unsigned allocation_action, send_allocations, send_frees;
static size_t allocation_size;
static size_t free_internal, largest_internal;
static void *allocated_sends[4];
static unsigned opcode;
static bool fin;
static int payload;
static const char *read_data;
static int read_size;
static int poll_result;

static bool failed(const char *name) { return failure && !strcmp(failure, name); }
void test_task_enter(portMUX_TYPE *lock) { assert(lock->depth == 0); ++lock->depth; }
void test_task_exit(portMUX_TYPE *lock) { assert(lock->depth == 1); --lock->depth; }
int64_t esp_timer_get_time(void) { return now; }
size_t heap_caps_get_free_size(uint32_t caps) { assert(caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)); return free_internal; }
size_t heap_caps_get_largest_free_block(uint32_t caps) { assert(caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)); return largest_internal; }
BaseType_t xTaskCreate(void (*function)(void *), const char *name, uint32_t stack, void *user, UBaseType_t priority, void *handle)
{
    assert(!strcmp(name, "linx_gateway") && stack == 6144 && !user && priority == 4 && !handle);
    ++tasks; task_function = function;
    return failed("task") ? pdFALSE : pdPASS;
}
void vTaskDelete(void *task) { assert(!task); ++task_deletes; }
void vTaskDelay(TickType_t ticks) { assert(ticks == TICK_MS); now += ticks * 1000LL; longjmp(worker_end, 1); }
bool linx_device_network_acquire(void) { ++acquisitions; return !failed("acquire"); }
void linx_device_network_release(void) { ++releases; }
bool linx_device_network_poll(linx_device_network_state_t *state) { *state = network; return true; }
void linx_device_network_closed(uint32_t epoch) { ++acknowledgements; acknowledged = epoch; }
esp_err_t esp_crt_bundle_attach(void *config) { (void)config; return ESP_OK; }
esp_transport_handle_t esp_transport_ssl_init(void)
{
    ++ssl_inits;
    if (failed("ssl")) return NULL;
    assert(!mock_ssl.live); mock_ssl.live = true; return &mock_ssl;
}
esp_transport_handle_t esp_transport_tcp_init(void)
{
    ++tcp_inits;
    if (failed("tcp")) return NULL;
    assert(!mock_tcp.live); mock_tcp.live = true; return &mock_tcp;
}
void esp_transport_ssl_crt_bundle_attach(esp_transport_handle_t handle, esp_err_t (*attach)(void *))
{ assert(handle == &mock_ssl && attach == esp_crt_bundle_attach); ++certificate_attaches; }
esp_transport_handle_t esp_transport_ws_init(esp_transport_handle_t parent)
{
    assert(parent == (tcp_inits ? &mock_tcp : &mock_ssl));
    if (failed("ws")) return NULL;
    assert(!mock_ws.live); mock_ws.live = true; return &mock_ws;
}
esp_err_t esp_transport_ws_set_config(esp_transport_handle_t handle, const esp_transport_ws_config_t *config)
{
    assert(handle == &mock_ws && !strcmp(config->ws_path, expected_path));
    assert(config->propagate_control_frames && !strcmp(config->auth, "Bearer device-token"));
    assert(config->user_agent && !config->headers);
    return failed("config") ? ESP_FAIL : ESP_OK;
}
int esp_transport_connect(esp_transport_handle_t handle, const char *host, int port, int timeout)
{
    assert(handle == &mock_ws && !strcmp(host, expected_host) && port == expected_port && timeout == CONNECT_TIMEOUT_MS);
    ++connects; return failed("connect") ? -1 : 0;
}
int esp_transport_poll_read(esp_transport_handle_t handle, int timeout)
{ assert(handle == &mock_ws && timeout == 0); return failed("poll") ? -1 : poll_result; }
int esp_transport_read(esp_transport_handle_t handle, char *buffer, int size, int timeout)
{
    assert(handle == &mock_ws && size == RX_CHUNK_SIZE && timeout == IO_TIMEOUT_MS);
    poll_result = 0;
    if (failed("read")) return -1;
    if (read_size > 0) memcpy(buffer, read_data, (size_t)read_size);
    return read_size;
}
ws_transport_opcodes_t esp_transport_ws_get_read_opcode(esp_transport_handle_t handle) { assert(handle == &mock_ws); return (ws_transport_opcodes_t)opcode; }
int esp_transport_ws_get_read_payload_len(esp_transport_handle_t handle) { assert(handle == &mock_ws); return payload; }
bool esp_transport_ws_get_fin_flag(esp_transport_handle_t handle) { assert(handle == &mock_ws); return fin; }
int esp_transport_ws_send_raw(esp_transport_handle_t handle, ws_transport_opcodes_t kind, const char *data, int size, int timeout)
{
    assert(handle == &mock_ws && (kind & WS_TRANSPORT_OPCODES_FIN) && timeout == IO_TIMEOUT_MS);
    assert(size >= 0 && (size_t)size <= sizeof(sent_data) - 1U);
    if ((kind & 15U) == WS_TRANSPORT_OPCODES_PONG) ++pongs;
    else ++sends;
    if (deinit_on_send) {
        deinit_on_send = false;
        linx_gateway_transport_deinit();
        assert(outgoing == data && outgoing_size == (size_t)size);
    }
    if (size) memcpy(sent_data, data, (size_t)size);
    sent_data[size] = 0;
    return failed("send") ? (size ? size - 1 : -1) : size;
}
int esp_transport_close(esp_transport_handle_t handle) { assert(handle == &mock_ws && handle->live); ++closes; return 0; }
esp_err_t esp_transport_destroy(esp_transport_handle_t handle) { assert(handle->live); handle->live = false; ++destroys; return ESP_OK; }
static void *mock_realloc(void *pointer, size_t size) { return alloc_failed ? NULL : realloc(pointer, size); }

static void *mock_malloc(size_t size)
{
    assert(mailbox_lock.depth == 0);
    allocation_size = size;
    if (send_alloc_failed) return NULL;
    void *buffer = malloc(size); assert(buffer);
    unsigned slot = 0; while (slot < 4 && allocated_sends[slot]) ++slot;
    assert(slot < 4); allocated_sends[slot] = buffer; ++send_allocations;
    unsigned action = allocation_action; allocation_action = 0;
    if (action == 1) close_connection();
    if (action == 2) linx_gateway_transport_deinit();
    if (action == 3) atomic_fetch_add(&public_generation, 1U);
    if (action == 4) assert(linx_gateway_transport_send(connection_generation, "inner", 5, NULL));
    return buffer;
}
static void mock_free(void *pointer)
{
    assert(mailbox_lock.depth == 0);
    for (unsigned slot = 0; slot < 4; ++slot) {
        if (pointer && allocated_sends[slot] == pointer) {
            allocated_sends[slot] = NULL; ++send_frees; break;
        }
    }
    free(pointer);
}

static linx_gateway_transport_event_t take(linx_gateway_transport_event_type_t type)
{
    linx_gateway_transport_event_t event;
    assert(linx_gateway_transport_poll(&event) && event.type == type);
    assert(!linx_gateway_transport_poll(&event));
    return event;
}
static void reset(void)
{
    linx_gateway_transport_release();
    atomic_store(&wanted, false);
    event_pending = false;
    transport_tick();
    event_pending = false;
    transport_tick();
    assert(!mock_ssl.live && !mock_tcp.live && !mock_ws.live && !incoming && !outgoing);
    assert(send_allocations == send_frees);
    atomic_store(&started, false); atomic_store(&wanted, false);
    atomic_store(&retry_requested, false); atomic_store(&connected, false); atomic_store(&public_generation, 0);
    frame_ready = frame_borrowed = event_pending = disconnect_pending = network_acquired = config_reported = false;
    connection_generation = connection_epoch = 0;
    memset(connection_mac, 0, sizeof(connection_mac));
    outgoing_size = 0; backoff_ms = 1000; reconnect_at = 0; last_network_error = 0; task_deletes = 0;
    now = 0; failure = NULL; alloc_failed = send_alloc_failed = deinit_on_send = false; poll_result = 0;
    allocation_action = send_allocations = send_frees = 0; allocation_size = 0;
    free_internal = 256U * 1024U; largest_internal = 128U * 1024U;
    destroys = closes = connects = sends = pongs = acquisitions = releases = acknowledgements = tasks = 0;
    network = (linx_device_network_state_t){.epoch = 1};
    strcpy(network.mac_address, "AA:BB:CC:DD:EE:FF");
    test_gateway_enable = true; test_gateway_url = "wss://gateway.example.org/device"; test_gateway_token = "device-token";
    test_gateway_allow_ws = false; ssl_inits = tcp_inits = certificate_attaches = 0;
    expected_host = "gateway.example.org"; expected_path = "/device"; expected_port = 443;
}
static void connect_ready(void)
{
    assert(linx_gateway_transport_init());
    transport_tick(); assert(acquisitions == 1 && !websocket);
    network.ready = true;
    transport_tick();
    linx_gateway_transport_event_t event = take(LINX_GATEWAY_TRANSPORT_CONNECTED);
    assert(event.generation && !strcmp(event.mac_address, network.mac_address));
}
static void supply(unsigned kind, bool final, int total, const char *data, int size)
{ opcode = kind; fin = final; payload = total; read_data = data; read_size = size; poll_result = 1; transport_tick(); }

static void test_configuration(void)
{
    reset(); assert(!linx_gateway_transport_poll(NULL));
    assert(!linx_gateway_transport_send(1, NULL, 1, NULL));
    failure = "task"; assert(!linx_gateway_transport_init() && !atomic_load(&wanted));
    failure = NULL; assert(linx_gateway_transport_init() && linx_gateway_transport_init() && tasks == 2);
    const char *urls[] = {"", "ws://localhost/device", "wss://", "https://gateway.example.org/device"};
    for (size_t i = 0; i < sizeof(urls)/sizeof(urls[0]); ++i) {
        test_gateway_url = urls[i]; linx_gateway_transport_retry(); transport_tick();
        take(LINX_GATEWAY_TRANSPORT_NOT_CONFIGURED); transport_tick(); assert(!event_pending && !acquisitions);
    }
    test_gateway_url = "wss://gateway.example.org/device";
    const char *tokens[] = {"", "has space", "newline\r\n", "\x7f", "\x80"};
    for (size_t i = 0; i < sizeof(tokens)/sizeof(tokens[0]); ++i) {
        test_gateway_token = tokens[i]; linx_gateway_transport_retry(); transport_tick(); take(LINX_GATEWAY_TRANSPORT_NOT_CONFIGURED);
    }
    char large[1026]; memset(large, 't', sizeof(large)); large[1025] = 0;
    test_gateway_token = large; linx_gateway_transport_retry(); transport_tick(); take(LINX_GATEWAY_TRANSPORT_NOT_CONFIGURED);
    test_gateway_token = "device-token"; test_gateway_enable = false; linx_gateway_transport_retry(); transport_tick(); take(LINX_GATEWAY_TRANSPORT_NOT_CONFIGURED);
    test_gateway_enable = true; failure = "acquire"; linx_gateway_transport_retry(); transport_tick(); take(LINX_GATEWAY_TRANSPORT_ERROR);
    assert(!network_acquired); failure = NULL;
    transport_tick(); network.error = LINX_VOICE_WIFI_FAILED; transport_tick(); take(LINX_GATEWAY_TRANSPORT_ERROR);
    transport_tick(); assert(!event_pending);
    network.provisioning = true; network.error = LINX_VOICE_TIME_FAILED; transport_tick(); assert(!event_pending);
    network.provisioning = false; transport_tick(); take(LINX_GATEWAY_TRANSPORT_ERROR);
}
static void test_connect_failures(void)
{
    for (unsigned plain = 0; plain < 2; ++plain) {
        const char *errors[] = {plain ? "tcp" : "ssl", "ws", "config", "connect"};
        for (size_t i = 0; i < sizeof(errors)/sizeof(errors[0]); ++i) {
            reset();
            if (plain) {
                test_gateway_allow_ws = true; test_gateway_url = "ws://gateway.example.org/device"; expected_port = 80;
            }
            assert(linx_gateway_transport_init()); transport_tick(); network.ready = true;
            failure = errors[i]; transport_tick(); take(LINX_GATEWAY_TRANSPORT_ERROR);
            assert(!mock_ssl.live && !mock_tcp.live && !mock_ws.live);
            if (disconnect_pending) { transport_tick(); take(LINX_GATEWAY_TRANSPORT_DISCONNECTED); }
            failure = NULL; transport_tick(); assert(!websocket);
            now = reconnect_at; transport_tick(); take(LINX_GATEWAY_TRANSPORT_CONNECTED);
            assert(linx_gateway_transport_init());
        }
    }
}
static void test_ws_configuration(void)
{
    reset(); test_gateway_url = "ws://124.220.162.10:8787/v1/devices/connect";
    assert(!config_valid() && !connect_gateway() && !ssl_inits && !tcp_inits);
    test_gateway_allow_ws = true;
    expected_host = "124.220.162.10"; expected_path = "/v1/devices/connect"; expected_port = 8787;
    assert(config_valid()); connect_ready();
    assert(tcp_inits == 1 && !ssl_inits && !certificate_attaches && mock_tcp.live);
    reset(); test_gateway_allow_ws = true; connect_ready();
    assert(ssl_inits == 1 && !tcp_inits && certificate_attaches == 1 && mock_ssl.live);
}
static void test_send_and_lifecycle(void)
{
    reset(); connect_ready();
    assert(!linx_gateway_transport_send(1, "", 0, NULL));
    assert(!linx_gateway_transport_send(1, "x", LINX_GATEWAY_SEND_CAPACITY + 1, NULL));
    assert(!linx_gateway_transport_send(99, "{}", 2, NULL));
    assert(linx_gateway_transport_send(connection_generation, "{\"id\":1}", 8, NULL));
    assert(!linx_gateway_transport_send(connection_generation, "{}", 2, NULL));
    transport_tick(); assert(sends == 1 && !strcmp(sent_data, "{\"id\":1}"));
    failure = "send"; assert(linx_gateway_transport_send(connection_generation, "{}", 2, NULL)); transport_tick();
    take(LINX_GATEWAY_TRANSPORT_ERROR); assert(!outgoing_size && closes == 1 && destroys == 2);
    transport_tick(); take(LINX_GATEWAY_TRANSPORT_DISCONNECTED);
    failure = NULL; linx_gateway_transport_retry(); transport_tick(); take(LINX_GATEWAY_TRANSPORT_CONNECTED);
    uint32_t previous = connection_generation;
    network.ready = false; ++network.epoch; transport_tick(); take(LINX_GATEWAY_TRANSPORT_DISCONNECTED);
    assert(acknowledged == network.epoch && !websocket);
    network.ready = true; transport_tick(); take(LINX_GATEWAY_TRANSPORT_CONNECTED); assert(connection_generation != previous);
    network.provisioning = true; transport_tick(); take(LINX_GATEWAY_TRANSPORT_DISCONNECTED);
    network.provisioning = false; transport_tick(); take(LINX_GATEWAY_TRANSPORT_CONNECTED);
    ++network.epoch; transport_tick(); take(LINX_GATEWAY_TRANSPORT_DISCONNECTED);
    transport_tick(); take(LINX_GATEWAY_TRANSPORT_CONNECTED);
    linx_gateway_transport_deinit(); transport_tick(); take(LINX_GATEWAY_TRANSPORT_DISCONNECTED); transport_tick();
    assert(!network_acquired && releases == 1 && !linx_gateway_transport_send(previous, "{}", 2, NULL));
}
static void test_frames(void)
{
    reset(); connect_ready();
    supply(1, false, 3, "abc", 2); assert(frame_active && !event_pending);
    supply(1, false, 3, "c", 1); assert(fragmented && !frame_active);
    supply(9, true, 2, "p", 1); supply(9, true, 2, "q", 1); assert(pongs == 1 && !strcmp(sent_data, "pq"));
    supply(10, true, 0, "", 0);
    supply(0, true, 3, "def", 3);
    linx_gateway_transport_event_t event = take(LINX_GATEWAY_TRANSPORT_FRAME);
    assert(event.size == 6 && !strcmp(event.data, "abcdef"));
    const char *held = event.data;
    supply(1, true, 1, "z", 1); assert(!strcmp(held, "abcdef") && !event_pending);
    linx_gateway_transport_release(); transport_tick();
    event = take(LINX_GATEWAY_TRANSPORT_FRAME); assert(event.size == 1 && !strcmp(event.data, "z"));
    linx_gateway_transport_deinit(); transport_tick();
    assert(incoming == event.data && !strcmp(event.data, "z"));
    take(LINX_GATEWAY_TRANSPORT_DISCONNECTED); transport_tick(); assert(incoming);
    linx_gateway_transport_release(); linx_gateway_transport_release(); transport_tick(); assert(!incoming);
    reset(); connect_ready(); supply(1, true, 0, "", 0);
    event = take(LINX_GATEWAY_TRANSPORT_FRAME); assert(!event.size && event.data[0] == 0); linx_gateway_transport_release();
    char block[1024]; memset(block, 'x', sizeof(block));
    for (unsigned i = 0; i < 64; ++i) supply(1, true, 65536, block, sizeof(block));
    event = take(LINX_GATEWAY_TRANSPORT_FRAME); assert(event.size == 65536 && event.data[65535] == 'x' && event.data[65536] == 0);
    linx_gateway_transport_release(); supply(1, true, 65537, "x", 1); take(LINX_GATEWAY_TRANSPORT_ERROR);
}
static void test_receive_memory_release(void)
{
    for (unsigned held = 0; held < 2; ++held) {
        reset(); connect_ready(); supply(1, true, 4, "held", 4);
        linx_gateway_transport_event_t event = take(LINX_GATEWAY_TRANSPORT_FRAME);
        if (!held) linx_gateway_transport_release();
        unsigned previous_acknowledgements = acknowledgements;
        network.ready = false; ++network.epoch; transport_tick();
        take(LINX_GATEWAY_TRANSPORT_DISCONNECTED);
        if (held) {
            assert(incoming == event.data && !strcmp(event.data, "held"));
            assert(acknowledgements == previous_acknowledgements);
            linx_gateway_transport_release(); transport_tick();
        }
        assert(!incoming && !incoming_capacity && !incoming_size);
        assert(acknowledged == network.epoch);
    }
}

static void test_infrastructure_headroom(void)
{
    for (unsigned scenario = 0; scenario < 5; ++scenario) {
        reset(); connect_ready();
        free_internal = INFRASTRUCTURE_FREE_BYTES + 4U;
        largest_internal = INFRASTRUCTURE_BLOCK_BYTES + 4U;
        if (scenario == 0) free_internal = INFRASTRUCTURE_FREE_BYTES - 1U;
        if (scenario == 1) free_internal = INFRASTRUCTURE_FREE_BYTES + 3U;
        if (scenario == 2) largest_internal = INFRASTRUCTURE_BLOCK_BYTES - 1U;
        if (scenario == 3) largest_internal = INFRASTRUCTURE_BLOCK_BYTES + 3U;
        assert(linx_gateway_transport_send(connection_generation, "test", 4, NULL) == (scenario == 4));
        assert(reserve_incoming(3) == (scenario == 4));
        if (scenario == 4) {
            free_internal = largest_internal = 0;
            assert(reserve_incoming(2)); /* 复用现有块不增长缓存。 */
            assert(!reserve_incoming(4));
        }
    }
}

static void test_bad_frames(void)
{
    struct { unsigned kind; bool final; int total; int size; } bad[] = {
        {0,true,1,1}, {2,true,1,1}, {3,true,1,1}, {8,true,1,1}, {8,false,0,0},
        {9,false,0,0}, {9,true,126,1}, {11,true,0,0}, {1,true,0,1}, {256,true,0,0}, {1,true,-1,0}, {8,true,0,0}
    };
    for (size_t i = 0; i < sizeof(bad)/sizeof(bad[0]); ++i) {
        reset(); connect_ready(); supply(bad[i].kind,bad[i].final,bad[i].total,"x",bad[i].size);
        take(LINX_GATEWAY_TRANSPORT_ERROR);
    }
    const char *errors[] = {"poll", "read"};
    for (size_t i = 0; i < sizeof(errors)/sizeof(errors[0]); ++i) {
        reset(); connect_ready(); failure = errors[i]; supply(1,true,1,"x",1); take(LINX_GATEWAY_TRANSPORT_ERROR);
    }
    reset(); connect_ready(); alloc_failed = true; supply(1,true,1,"x",1); take(LINX_GATEWAY_TRANSPORT_ERROR);
    reset(); connect_ready(); supply(1,false,1,"x",1); supply(1,true,1,"y",1); take(LINX_GATEWAY_TRANSPORT_ERROR);
    reset(); connect_ready(); supply(1,true,2,"x",1); supply(0,true,2,"y",1); take(LINX_GATEWAY_TRANSPORT_ERROR);
    reset(); connect_ready(); supply(1,true,2,"x",1); supply(1,false,2,"y",1); take(LINX_GATEWAY_TRANSPORT_ERROR);
    reset(); connect_ready();
    char full[1024]; memset(full, 'x', sizeof(full));
    for (unsigned i = 0; i < 64; ++i) supply(1,false,65536,full,sizeof(full));
    supply(0,true,1,"x",1); take(LINX_GATEWAY_TRANSPORT_ERROR);
    reset(); connect_ready(); supply(1,false,1,"x",1); now = frame_deadline; transport_tick(); take(LINX_GATEWAY_TRANSPORT_ERROR);
    reset(); connect_ready(); failure = "send"; supply(9,true,1,"x",1); take(LINX_GATEWAY_TRANSPORT_ERROR);
    reset(); connect_ready(); failure = "send"; supply(9,true,0,"",0); take(LINX_GATEWAY_TRANSPORT_ERROR);
}
static void test_dynamic_send_slot(void)
{
    reset(); connect_ready();
    send_alloc_failed = true;
    assert(!linx_gateway_transport_send(connection_generation, "raw", 3, NULL));
    assert(allocation_size == 3 && !outgoing && !outgoing_size && sends == 0);
    send_alloc_failed = false;
    char raw[] = "exact bytes";
    assert(linx_gateway_transport_send(connection_generation, raw, sizeof(raw) - 1U, NULL));
    assert(allocation_size == sizeof(raw) - 1U && outgoing != raw && send_allocations == 1);
    memset(raw, 'x', sizeof(raw) - 1U);
    transport_tick();
    assert(!strcmp(sent_data, "exact bytes") && !outgoing && !outgoing_size && send_frees == 1);
    char maximum[LINX_GATEWAY_SEND_CAPACITY]; memset(maximum, 1, sizeof(maximum));
    assert(linx_gateway_transport_send(connection_generation, maximum, sizeof(maximum), NULL));
    assert(allocation_size == sizeof(maximum)); transport_tick();
    assert(sends == 2 && !outgoing && send_frees == 2);

    for (unsigned action = 1; action <= 4; ++action) {
        reset(); connect_ready(); allocation_action = action;
        assert(!linx_gateway_transport_send(connection_generation, "outer", 5, NULL));
        if (action == 4) {
            assert(outgoing_size == 5 && send_allocations == 2 && send_frees == 1);
            transport_tick(); assert(!strcmp(sent_data, "inner"));
        } else assert(!outgoing && send_allocations == send_frees);
        reset();
    }

    connect_ready();
    supply(1, true, 4, "held", 4);
    linx_gateway_transport_event_t event = take(LINX_GATEWAY_TRANSPORT_FRAME);
    const char *held = event.data;
    assert(linx_gateway_transport_send(connection_generation, "queued", 6, NULL));
    transport_tick(); assert(sends == 0 && outgoing && !strcmp(held, "held"));
    linx_gateway_transport_deinit(); transport_tick();
    assert(!outgoing && !outgoing_size && send_allocations == send_frees && !strcmp(held, "held"));
    take(LINX_GATEWAY_TRANSPORT_DISCONNECTED); linx_gateway_transport_release(); transport_tick();
    assert(!incoming);

    reset(); connect_ready(); deinit_on_send = true;
    assert(linx_gateway_transport_send(connection_generation, "in-flight", 9, NULL));
    transport_tick(); assert(!outgoing && send_allocations == send_frees && sends == 1);
    transport_tick(); take(LINX_GATEWAY_TRANSPORT_DISCONNECTED); transport_tick();
    assert(!network_acquired);

    reset(); connect_ready();
    assert(linx_gateway_transport_send(connection_generation, "stale", 5, NULL));
    ++connection_generation; transport_tick();
    take(LINX_GATEWAY_TRANSPORT_ERROR);
    assert(!outgoing && !outgoing_size && sends == 0 && send_allocations == send_frees);
}

static void test_mailbox_backoff_and_worker(void)
{
    reset(); assert(linx_gateway_transport_init()); transport_tick(); network.ready = true; transport_tick();
    assert(event_pending); supply(1,true,2,"{}",2); assert(!frame_ready);
    take(LINX_GATEWAY_TRANSPORT_CONNECTED); transport_tick(); take(LINX_GATEWAY_TRANSPORT_FRAME);
    network.ready = false; ++network.epoch; transport_tick();
    take(LINX_GATEWAY_TRANSPORT_DISCONNECTED); linx_gateway_transport_release();
    network.ready = true; transport_tick(); take(LINX_GATEWAY_TRANSPORT_CONNECTED);
    failure = "poll"; transport_tick(); take(LINX_GATEWAY_TRANSPORT_ERROR);
    transport_tick(); take(LINX_GATEWAY_TRANSPORT_DISCONNECTED);
    failure = "ssl";
    for (unsigned i = 0; i < 8; ++i) { now = reconnect_at; transport_tick(); take(LINX_GATEWAY_TRANSPORT_ERROR); }
    assert(backoff_ms == BACKOFF_MAX_MS);
    failure = NULL; connection_generation = UINT32_MAX; linx_gateway_transport_retry(); transport_tick();
    assert(take(LINX_GATEWAY_TRANSPORT_CONNECTED).generation == 1);
    if (!setjmp(worker_end)) task_function(NULL);
    assert(now > 0);
    linx_gateway_transport_deinit();
    if (!setjmp(worker_end)) task_function(NULL);
    take(LINX_GATEWAY_TRANSPORT_DISCONNECTED);
    task_function(NULL); assert(task_deletes == 1 && !atomic_load(&started));
    assert(linx_gateway_transport_init() && tasks == 2);
}
int main(void)
{
    test_dynamic_send_slot();
    test_configuration(); test_ws_configuration(); test_connect_failures(); test_send_and_lifecycle();
    test_frames(); test_receive_memory_release(); test_infrastructure_headroom(); test_bad_frames(); test_mailbox_backoff_and_worker(); reset();
    puts("Gateway WS/WSS：显式明文开关、TLS、分帧、容量、借出、退避与网络 epoch 测试通过");
    return 0;
}
