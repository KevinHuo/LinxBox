/* 真实网页及解析实现；只替换 HTTP、互斥与分配边界，不接触本机网络。 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sdk_stubs.h"

static void *portal_alloc(size_t count, size_t size);
static void portal_free(void *memory);
#define calloc portal_alloc
#define free portal_free
#include "../main/linx_wifi_portal.c"
#undef calloc
#undef free

struct test_semaphore { bool locked; };
static struct test_semaphore semaphore;
static httpd_uri_t handlers[3];
static size_t handler_count, allocation_count, freed_count, lock_failures, mutex_deletes;
static int registration_failure, registration_calls, reply_failure, reply_calls, json_failure;
static bool allocation_failure, mutex_failure, start_failure, stop_failure, header_failure;
static bool server_live, expect_cleared_free;
static size_t stop_calls, json_calls;
static int64_t clock_us;
static size_t lock_calls, lock_failure_at;
static httpd_config_t server_config;
static bool sockets_live[32], sockets_interrupted[32];
static size_t shutdown_calls[32], close_calls[32], context_releases;
static bool socket_shutdown_failure, socket_close_failure, open_during_stop;

static void sdk_accept_failure_close(int socket)
{
    /* SDK sess_new 已触发 close_fn，accept 失败尾部仍会关闭这个 fd。 */
    assert(sockets_live[socket] && !semaphore.locked);
    sockets_live[socket] = false;
    ++close_calls[socket];
}

typedef struct {
    const char *origin;
    const char *content_type;
    const char *body;
    size_t offset;
    size_t chunk;
    int recv_result;
    int64_t receive_delay_us;
    size_t cancel_after;
    char status[64];
    char type[64];
    char response[12000];
    size_t cache_headers, close_headers, nosniff_headers;
} transaction_t;

static const linx_wifi_network_t NETWORKS[] = {
    {.ssid = "家庭网络", .rssi = -32, .security = LINX_WIFI_PERSONAL},
    {.ssid = "Free", .rssi = -61, .security = LINX_WIFI_OPEN},
    {.ssid = "Enterprise", .rssi = -71, .security = LINX_WIFI_UNSUPPORTED},
    {.ssid = "<script>\"\\\n", .rssi = -80, .security = LINX_WIFI_PERSONAL},
};

static void reset(void)
{
    assert(!server_live);
    assert(allocation_count == freed_count);
    memset(handlers, 0, sizeof(handlers));
    handler_count = 0;
    registration_calls = reply_calls = 0;
    registration_failure = reply_failure = -1;
    json_failure = -1;
    json_calls = 0;
    allocation_failure = mutex_failure = start_failure = stop_failure = header_failure = false;
    expect_cleared_free = true;
    lock_failures = 0;
    clock_us = 0;
    lock_calls = 0;
    lock_failure_at = SIZE_MAX;
    memset(&server_config, 0, sizeof(server_config));
    memset(sockets_live, 0, sizeof(sockets_live));
    memset(sockets_interrupted, 0, sizeof(sockets_interrupted));
    memset(shutdown_calls, 0, sizeof(shutdown_calls));
    memset(close_calls, 0, sizeof(close_calls));
    socket_shutdown_failure = socket_close_failure = open_during_stop = false;
}

static void *portal_alloc(size_t count, size_t size)
{
    if (allocation_failure) return NULL;
    ++allocation_count;
    return calloc(count, size);
}

static void portal_free(void *memory)
{
    if (expect_cleared_free) {
        const unsigned char *bytes = memory;
        for (size_t i = 0; i < sizeof(linx_wifi_portal_t); ++i) assert(bytes[i] == 0);
    }
    ++freed_count;
    free(memory);
}

static void *json_alloc(size_t size)
{
    if ((int)json_calls++ == json_failure) return NULL;
    return malloc(size);
}

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    if (mutex_failure) return NULL;
    semaphore.locked = false;
    return &semaphore;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t handle, TickType_t wait)
{
    assert(handle == &semaphore && wait == portMAX_DELAY && !handle->locked);
    if (lock_calls++ == lock_failure_at) return pdFALSE;
    if (lock_failures > 0U) { --lock_failures; return pdFALSE; }
    handle->locked = true;
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t handle)
{
    assert(handle == &semaphore && handle->locked);
    handle->locked = false;
    return pdTRUE;
}

void vSemaphoreDelete(SemaphoreHandle_t handle)
{
    assert(handle == &semaphore && !handle->locked && !server_live);
    ++mutex_deletes;
}

esp_err_t httpd_start(httpd_handle_t *handle, const httpd_config_t *config)
{
    assert(config->max_open_sockets == 2 && config->stack_size >= 4096 && config->stack_size <= 6144);
    assert(config->recv_wait_timeout == 2 && config->send_wait_timeout == 2);
    assert(config->max_uri_handlers == 3 && config->max_resp_headers == 3 && config->lru_purge_enable);
    assert(config->global_user_ctx != NULL && config->global_user_ctx_free_fn != NULL);
    assert(config->open_fn != NULL && config->close_fn != NULL);
    if (start_failure) return ESP_FAIL;
    server_config = *config;
    server_live = true;
    *handle = handlers;
    return ESP_OK;
}

esp_err_t httpd_stop(httpd_handle_t handle)
{
    assert(handle == handlers && server_live && !semaphore.locked);
    ++stop_calls;
    linx_wifi_portal_t *portal = server_config.global_user_ctx;
    /* 模拟处理函数之前卡在收头/发送的连接：必须先被 shutdown 唤醒。 */
    for (size_t i = 0; i < 2; ++i) {
        int socket = portal->clients[i];
        if (socket >= 0) assert(sockets_live[socket] && sockets_interrupted[socket]);
    }
    if (open_during_stop) {
        sockets_live[12] = true;
        assert(server_config.open_fn(handle, 12) == ESP_FAIL);
        server_config.close_fn(handle, 12);
        assert(sockets_live[12]);
        sdk_accept_failure_close(12);
        assert(!sockets_live[12]);
        open_during_stop = false;
    }
    if (stop_failure) return ESP_FAIL;
    for (size_t i = 0; i < 2; ++i) {
        if (portal->clients[i] >= 0) server_config.close_fn(handle, portal->clients[i]);
    }
    server_config.global_user_ctx_free_fn(server_config.global_user_ctx);
    ++context_releases;
    server_live = false;
    return ESP_OK;
}

void *httpd_get_global_user_ctx(httpd_handle_t handle)
{
    assert(handle == handlers && server_live);
    return server_config.global_user_ctx;
}

int test_wifi_socket_shutdown(int socket, int how)
{
    assert(socket >= 0 && socket < 32 && sockets_live[socket]);
    assert(how == SHUT_RDWR && semaphore.locked);
    sockets_interrupted[socket] = true;
    ++shutdown_calls[socket];
    return socket_shutdown_failure ? -1 : 0;
}

int test_wifi_socket_close(int socket)
{
    assert(socket >= 0 && socket < 32 && sockets_live[socket] && semaphore.locked);
    sockets_live[socket] = false;
    ++close_calls[socket];
    return socket_close_failure ? -1 : 0;
}

esp_err_t httpd_register_uri_handler(httpd_handle_t handle, const httpd_uri_t *uri)
{
    assert(handle == handlers && server_live && handler_count < 3);
    if (registration_calls++ == registration_failure) return ESP_FAIL;
    handlers[handler_count++] = *uri;
    return ESP_OK;
}

static const char *header(httpd_req_t *request, const char *name)
{
    transaction_t *transaction = request->test_data;
    if (strcmp(name, "Origin") == 0) return transaction->origin;
    assert(strcmp(name, "Content-Type") == 0);
    return transaction->content_type;
}

size_t httpd_req_get_hdr_value_len(httpd_req_t *request, const char *name)
{
    const char *value = header(request, name);
    return value == NULL ? 0U : strlen(value);
}

esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *request, const char *name, char *value, size_t size)
{
    if (header_failure) return ESP_FAIL;
    const char *source = header(request, name);
    assert(source != NULL && strlen(source) < size);
    strcpy(value, source);
    return ESP_OK;
}

int httpd_req_recv(httpd_req_t *request, char *buffer, size_t size)
{
    transaction_t *transaction = request->test_data;
    if (transaction->recv_result <= 0) return transaction->recv_result;
    if (size > transaction->chunk) size = transaction->chunk;
    memcpy(buffer, transaction->body + transaction->offset, size);
    transaction->offset += size;
    clock_us += transaction->receive_delay_us;
    if (transaction->cancel_after > 0U && transaction->offset >= transaction->cancel_after) {
        linx_wifi_portal_t *portal = request->user_ctx;
        assert(lock(portal));
        portal->closing = true;
        xSemaphoreGive(portal->mutex);
    }
    return (int)size;
}

int64_t esp_timer_get_time(void)
{
    return clock_us;
}

static bool reply_failed(void)
{
    return reply_calls++ == reply_failure;
}

esp_err_t httpd_resp_set_status(httpd_req_t *request, const char *status)
{
    transaction_t *transaction = request->test_data;
    strcpy(transaction->status, status);
    return reply_failed() ? ESP_FAIL : ESP_OK;
}

esp_err_t httpd_resp_set_type(httpd_req_t *request, const char *type)
{
    transaction_t *transaction = request->test_data;
    strcpy(transaction->type, type);
    return reply_failed() ? ESP_FAIL : ESP_OK;
}

esp_err_t httpd_resp_set_hdr(httpd_req_t *request, const char *name, const char *value)
{
    transaction_t *transaction = request->test_data;
    if (strcmp(name, "Cache-Control") == 0) {
        assert(strcmp(value, "no-store") == 0); ++transaction->cache_headers;
    } else if (strcmp(name, "Connection") == 0) {
        assert(strcmp(value, "close") == 0); ++transaction->close_headers;
    } else {
        assert(strcmp(name, "X-Content-Type-Options") == 0 && strcmp(value, "nosniff") == 0);
        ++transaction->nosniff_headers;
    }
    return reply_failed() ? ESP_FAIL : ESP_OK;
}

esp_err_t httpd_resp_send(httpd_req_t *request, const char *buffer, ssize_t size)
{
    transaction_t *transaction = request->test_data;
    assert(size == HTTPD_RESP_USE_STRLEN && strlen(buffer) < sizeof(transaction->response));
    strcpy(transaction->response, buffer);
    return reply_failed() ? ESP_FAIL : ESP_OK;
}

static linx_wifi_portal_t *start(const linx_wifi_network_t *networks, size_t count)
{
    linx_wifi_portal_t *portal = NULL;
    assert(linx_wifi_portal_start(&portal, networks, count) == ESP_OK);
    assert(portal != NULL && handler_count == 3);
    return portal;
}

static transaction_t request(linx_wifi_portal_t *portal, size_t index, const char *body,
                              size_t size, const char *origin, const char *content_type)
{
    transaction_t transaction = {.body = body, .origin = origin, .content_type = content_type,
                                  .chunk = 7U, .recv_result = 1};
    httpd_req_t request = {.user_ctx = portal, .content_len = size, .test_data = &transaction};
    esp_err_t result = handlers[index].handler(&request);
    assert(result == (transaction.offset < size ? ESP_FAIL : ESP_OK));
    assert(transaction.cache_headers == 1 && transaction.nosniff_headers == 1 && transaction.close_headers == 1);
    return transaction;
}

static transaction_t post(linx_wifi_portal_t *portal, const char *body, int status)
{
    transaction_t transaction = request(portal, 2, body, strlen(body), "http://192.168.4.1", "application/json");
    assert(atoi(transaction.status) == status);
    assert(strcmp(transaction.response, "{}") == 0);
    return transaction;
}

static void test_lifecycle(void)
{
    reset();
    linx_wifi_portal_t *portal = NULL;
    assert(linx_wifi_portal_start(NULL, NETWORKS, 1) == ESP_FAIL);
    assert(linx_wifi_portal_start(&portal, NULL, 1) == ESP_FAIL);
    assert(linx_wifi_portal_start(&portal, NETWORKS, 9) == ESP_FAIL);
    linx_wifi_network_t invalid = NETWORKS[0];
    invalid.ssid[0] = '\0';
    assert(linx_wifi_portal_start(&portal, &invalid, 1) == ESP_FAIL);
    memset(invalid.ssid, 'a', sizeof(invalid.ssid));
    assert(linx_wifi_portal_start(&portal, &invalid, 1) == ESP_FAIL);
    invalid = NETWORKS[0]; invalid.security = (linx_wifi_security_t)-1;
    assert(linx_wifi_portal_start(&portal, &invalid, 1) == ESP_FAIL);
    invalid.security = LINX_WIFI_UNSUPPORTED + 1;
    assert(linx_wifi_portal_start(&portal, &invalid, 1) == ESP_FAIL);
    allocation_failure = true;
    assert(linx_wifi_portal_start(&portal, NETWORKS, 1) == ESP_ERR_NO_MEM);
    allocation_failure = false; mutex_failure = true;
    assert(linx_wifi_portal_start(&portal, NETWORKS, 1) == ESP_ERR_NO_MEM);
    mutex_failure = false; start_failure = true;
    assert(linx_wifi_portal_start(&portal, NETWORKS, 1) == ESP_FAIL && portal == NULL);
    start_failure = false;
    for (int i = 0; i < 3; ++i) {
        reset(); registration_failure = i;
        assert(linx_wifi_portal_start(&portal, NETWORKS, 1) == ESP_FAIL && portal == NULL && !server_live);
    }
    reset(); registration_failure = 1; stop_failure = true;
    assert(linx_wifi_portal_start(&portal, NETWORKS, 1) == ESP_FAIL && portal != NULL && portal->closing);
    stop_failure = false;
    assert(linx_wifi_portal_stop(portal) == ESP_OK);
    assert(linx_wifi_portal_stop(NULL) == ESP_OK);
    reset(); portal = start(NULL, 0);
    char password[LINX_WIFI_PASSWORD_CAPACITY];
    linx_wifi_network_t network;
    assert(!linx_wifi_portal_poll(NULL, &network, password));
    assert(!linx_wifi_portal_poll(portal, NULL, password));
    assert(!linx_wifi_portal_poll(portal, &network, NULL));
    memset(password, 'x', sizeof(password));
    assert(!linx_wifi_portal_poll(portal, &network, password) && password[0] == '\0');
    lock_failures = 1;
    assert(!linx_wifi_portal_poll(portal, &network, password));
    linx_wifi_portal_status(NULL, LINX_WIFI_PORTAL_READY);
    linx_wifi_portal_status(portal, (linx_wifi_portal_state_t)-1);
    linx_wifi_portal_status(portal, LINX_WIFI_PORTAL_ERROR + 1);
    lock_failures = 1;
    linx_wifi_portal_status(portal, LINX_WIFI_PORTAL_ERROR);
    lock_failures = 1;
    assert(linx_wifi_portal_stop(portal) == ESP_FAIL && !portal->closing);
    assert(linx_wifi_portal_stop(portal) == ESP_OK);
}

static void test_page_and_status(void)
{
    reset(); linx_wifi_portal_t *portal = start(NETWORKS, 4);
    transaction_t page = request(portal, 0, "", 0, NULL, NULL);
    assert(strcmp(page.type, "text/html; charset=utf-8") == 0);
    assert(strstr(page.response, "type=\"password\"") != NULL);
    assert(strstr(page.response, "option.text=item.ssid") != NULL);
    assert(strstr(page.response, "innerHTML") == NULL && strstr(page.response, "localStorage") == NULL);
    assert(strstr(page.response, "<script src") == NULL && strstr(page.response, "https://") == NULL);
    for (size_t index = 0; index < 2U; ++index) {
        transaction_t rejection = request(portal, index, "unread", 6, NULL, NULL);
        assert(atoi(rejection.status) == 400 && rejection.offset == 0);
    }
    const char *preview = getenv("LINX_WIFI_PORTAL_PREVIEW");
    if (preview != NULL) {
        FILE *file = fopen(preview, "w"); assert(file != NULL);
        assert(fputs(page.response, file) >= 0); assert(fclose(file) == 0);
    }
    for (int state = LINX_WIFI_PORTAL_READY; state <= LINX_WIFI_PORTAL_ERROR; ++state) {
        linx_wifi_portal_status(portal, (linx_wifi_portal_state_t)state);
        transaction_t transaction = request(portal, 1, "", 0, NULL, NULL);
        assert(atoi(transaction.status) == 200);
        cJSON *root = cJSON_Parse(transaction.response);
        cJSON *networks = cJSON_GetObjectItemCaseSensitive(root, "networks");
        assert(cJSON_GetArraySize(networks) == 4);
        assert(strcmp(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(networks, 0), "ssid")->valuestring, NETWORKS[0].ssid) == 0);
        assert(strcmp(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(networks, 3), "ssid")->valuestring, NETWORKS[3].ssid) == 0);
        assert(cJSON_GetObjectItemCaseSensitive(root, "password") == NULL);
        cJSON_Delete(root);
    }
    json_calls = 0;
    transaction_t transaction = request(portal, 1, "", 0, NULL, NULL);
    size_t allocations = json_calls;
    assert(allocations > 20);
    for (size_t i = 0; i < allocations; ++i) {
        json_calls = 0; json_failure = (int)i;
        transaction = request(portal, 1, "", 0, NULL, NULL);
        assert(atoi(transaction.status) == 503);
    }
    json_failure = -1;
    for (int i = 0; i < 6; ++i) {
        reply_calls = 0; reply_failure = i;
        transaction = (transaction_t){0};
        httpd_req_t request = {.user_ctx = portal, .test_data = &transaction};
        assert(handlers[0].handler(&request) == ESP_FAIL);
        assert(reply_calls == i + 1);
    }
    reply_failure = -1; lock_failures = 1;
    httpd_req_t get = {.user_ctx = portal, .test_data = &transaction};
    assert(handlers[1].handler(&get) == ESP_FAIL);
    stop_failure = true;
    assert(linx_wifi_portal_stop(portal) == ESP_FAIL);
    transaction = request(portal, 1, "", 0, NULL, NULL);
    assert(atoi(transaction.status) == 503);
    linx_wifi_portal_status(portal, LINX_WIFI_PORTAL_SAVED);
    assert(portal->state == LINX_WIFI_PORTAL_ERROR);
    stop_failure = false;
    assert(linx_wifi_portal_stop(portal) == ESP_OK);
    reset(); portal = start(NULL, 0);
    transaction = request(portal, 1, "", 0, NULL, NULL);
    assert(strstr(transaction.response, "\"networks\":[]") != NULL);
    assert(linx_wifi_portal_stop(portal) == ESP_OK);
}

static void test_connect(void)
{
    reset(); linx_wifi_portal_t *portal = start(NETWORKS, 4);
    const char *valid = "{\"ssid\":\"家庭网络\",\"password\":\"password123\"}";
    post(portal, valid, 202);
    assert(strcmp(portal->password, "password123") == 0);
    post(portal, valid, 409);
    linx_wifi_network_t network;
    char password[LINX_WIFI_PASSWORD_CAPACITY];
    assert(linx_wifi_portal_poll(portal, &network, password));
    assert(strcmp(password, "password123") == 0 && strcmp(network.ssid, NETWORKS[0].ssid) == 0);
    for (size_t i = 0; i < sizeof(portal->password); ++i) assert(portal->password[i] == '\0');
    assert(!linx_wifi_portal_poll(portal, &network, password));
    post(portal, valid, 409);
    linx_wifi_portal_status(portal, LINX_WIFI_PORTAL_SAVED);
    post(portal, valid, 409);
    linx_wifi_portal_status(portal, LINX_WIFI_PORTAL_ERROR);
    post(portal, " { \"password\" : \"pass\\u0077ord\\/\\\\\\\"\", \"ssid\": \"\\u5bb6\\u5ead网络\" } \r\n", 202);
    assert(linx_wifi_portal_poll(portal, &network, password));
    assert(strcmp(password, "password/\\\"") == 0);
    linx_wifi_portal_status(portal, LINX_WIFI_PORTAL_ERROR);
    post(portal, "{\"ssid\":\"Free\",\"password\":\"\"}", 202);
    assert(linx_wifi_portal_poll(portal, &network, password) && network.security == LINX_WIFI_OPEN && password[0] == '\0');
    linx_wifi_portal_status(portal, LINX_WIFI_PORTAL_READY);
    transaction_t transaction = request(portal, 2, valid, strlen(valid), NULL, "application/json");
    assert(atoi(transaction.status) == 202);
    stop_failure = true;
    assert(linx_wifi_portal_stop(portal) == ESP_FAIL);
    for (size_t i = 0; i < sizeof(portal->password); ++i) assert(portal->password[i] == '\0');
    assert(!linx_wifi_portal_poll(portal, &network, password));
    post(portal, valid, 503);
    stop_failure = false;
    assert(linx_wifi_portal_stop(portal) == ESP_OK);
}

static void test_invalid(void)
{
    reset(); linx_wifi_portal_t *portal = start(NETWORKS, 4);
    const char *const invalid[] = {
        "", " ", "[]", "{", "{}", "{\"ssid\"}", "{\"ssid\":1}",
        "{\"ssid\":\"Free\"}", "{\"ssid\":\"Free\",\"password\":\"\",\"extra\":1}",
        "{\"ssid\":\"Free\",\"password\":\"\"}x", "{\"ssid\":\"Free\",\"password\":\"\"}{}",
        "{\"ssid\":\"Free\",\"ssid\":\"Free\"}", "{\"password\":\"\",\"password\":\"\"}",
        "{\"other\":\"Free\",\"password\":\"\"}", "{\"password1\":\"x\",\"ssid\":\"Free\"}",
        "{\"ssid\":\"\",\"password\":\"\"}", "{\"ssid\":\"missing\",\"password\":\"12345678\"}",
        "{\"ssid\":\"Enterprise\",\"password\":\"12345678\"}", "{\"ssid\":\"Free\",\"password\":\"12345678\"}",
        "{\"ssid\":\"家庭网络\",\"password\":\"short\"}", "{\"ssid\":\"Free\",\"password\":\"x\\u0000x\"}",
        "{\"ssid\":\"Free\\u0000other\",\"password\":\"\"}", "{\"ssid\":\"Free\\q\",\"password\":\"\"}",
        "{\"ssid\":\"Free\",\"password\":\"x\\q12345678\"}", "{\"ssid\":\"Free\",\"password\":\"x\\u00zz12345678\"}",
        "{\"ssid\":\"Free\",\"password\":\"x\\u12\"}", "{\"ssid\":\"Free\",\"password\":\"\\u4E0012345678\"}",
        "{\"ssid\":\"家庭网络\",\"password\":\"中文密码1234\"}", "{\"ssid\":\"Free\",\"password\":\"\\n12345678\"}",
        "{\"ssid\":\"Free\",\"password\":\"unclosed}", "{\"ssid\":\"Free\",\"password\":\"end\\",
        "{\"ssid\":\"Free\",\"password\":\"a\nb\"}", "{\"ssid\":\"Free\",\"password\":\"\177\"}",
        "{\"ssid\":\"123456789012345678901234567890123\",\"password\":\"\"}",
        "{\"ssid\":\"Free\",\"password\":\"123456789012345678901234567890123456789012345678901234567890123456789\"}",
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) post(portal, invalid[i], 400);
    const char *valid = "{\"ssid\":\"Free\",\"password\":\"\"}";
    transaction_t transaction = request(portal, 2, valid, strlen(valid), "http://evil.example", "application/json");
    assert(atoi(transaction.status) == 403);
    transaction = request(portal, 2, valid, strlen(valid), "null", "application/json");
    assert(atoi(transaction.status) == 403);
    char huge[514]; memset(huge, 'a', 513); huge[513] = '\0';
    transaction = request(portal, 2, valid, strlen(valid), huge, "application/json");
    assert(atoi(transaction.status) == 403);
    transaction = request(portal, 2, valid, strlen(valid), NULL, NULL);
    assert(atoi(transaction.status) == 415);
    transaction = request(portal, 2, valid, strlen(valid), NULL, "text/plain");
    assert(atoi(transaction.status) == 415);
    header_failure = true;
    transaction = request(portal, 2, valid, strlen(valid), NULL, "application/json");
    assert(atoi(transaction.status) == 415);
    header_failure = false;
    transaction = request(portal, 2, huge, 513, NULL, "application/json");
    assert(atoi(transaction.status) == 413 && transaction.offset == 0);
    huge[10] = '\0';
    transaction = request(portal, 2, huge, 512, NULL, "application/json");
    assert(atoi(transaction.status) == 400);
    huge[10] = 'a';
    transaction = request(portal, 2, huge, 512, NULL, "application/json");
    assert(atoi(transaction.status) == 400);
    for (int result = 0; result >= -1; --result) {
        transaction = (transaction_t){.content_type = "application/json", .body = valid, .recv_result = result};
        httpd_req_t request = {.user_ctx = portal, .content_len = strlen(valid), .test_data = &transaction};
        assert(handlers[2].handler(&request) == ESP_FAIL && atoi(transaction.status) == 400);
    }
    lock_failures = 1;
    post(portal, valid, 503);
    lock_failure_at = lock_calls + (strlen(valid) + 6U) / 7U * 2U;
    post(portal, valid, 400);
    lock_failure_at = SIZE_MAX;
    json_failure = 0; json_calls = 0;
    post(portal, valid, 400);
    json_failure = -1;
    assert(linx_wifi_portal_stop(portal) == ESP_OK);
}

static void test_receive_bounds(void)
{
    const char *valid = "{\"ssid\":\"家庭网络\",\"password\":\"password123\"}";
    for (int mode = 0; mode < 3; ++mode) {
        reset();
        linx_wifi_portal_t *portal = start(NETWORKS, 4);
        transaction_t transaction = {.body = valid, .content_type = "application/json",
                                      .chunk = 1U, .recv_result = 1};
        if (mode == 0) transaction.receive_delay_us = INT64_C(1000000);
        else if (mode == 1) transaction.cancel_after = 1;
        else { transaction.receive_delay_us = RECEIVE_TIMEOUT_US; transaction.chunk = strlen(valid); }
        httpd_req_t request = {.user_ctx = portal, .content_len = strlen(valid), .test_data = &transaction};
        assert(handlers[2].handler(&request) == (mode == 2 ? ESP_OK : ESP_FAIL));
        assert(atoi(transaction.status) == (mode == 1 ? 503 : 408));
        assert(transaction.offset == (mode == 0 ? 5U : mode == 1 ? 1U : strlen(valid)));
        assert(!portal->pending && portal->password[0] == '\0');
        assert(portal->state == LINX_WIFI_PORTAL_READY);
        assert(linx_wifi_portal_stop(portal) == ESP_OK);
    }
    reset();
    linx_wifi_portal_t *portal = start(NETWORKS, 4);
    transaction_t transaction = {.body = valid, .content_type = "application/json",
                                  .chunk = strlen(valid), .recv_result = 1,
                                  .receive_delay_us = RECEIVE_TIMEOUT_US - 1};
    httpd_req_t request = {.user_ctx = portal, .content_len = strlen(valid), .test_data = &transaction};
    assert(handlers[2].handler(&request) == ESP_OK && atoi(transaction.status) == 202);
    assert(portal->pending);
    assert(linx_wifi_portal_stop(portal) == ESP_OK);
    reset();
    portal = start(NETWORKS, 4);
    /* 两次时钟读之间到期：不再开始新的接收。 */
    assert(strcmp(receive_status(portal, -RECEIVE_TIMEOUT_US), "408 Request Timeout") == 0);
    assert(linx_wifi_portal_stop(portal) == ESP_OK);
}

static void accept_socket(int socket, esp_err_t expected)
{
    assert(!sockets_live[socket]);
    sockets_live[socket] = true;
    sockets_interrupted[socket] = false;
    esp_err_t result = server_config.open_fn(handlers, socket);
    assert(result == expected);
    if (result != ESP_OK) {
        server_config.close_fn(handlers, socket);
        assert(sockets_live[socket]);
        sdk_accept_failure_close(socket);
    }
}

static void test_socket_cancellation(void)
{
    reset();
    linx_wifi_portal_t *portal = start(NETWORKS, 4);
    accept_socket(10, ESP_OK);
    accept_socket(11, ESP_OK);
    accept_socket(12, ESP_FAIL);
    assert(close_calls[12] == 1 && portal->clients[0] == 10 && portal->clients[1] == 11);
    server_config.close_fn(handlers, 10);
    assert(portal->clients[0] == -1 && close_calls[10] == 1);
    accept_socket(10, ESP_OK);
    open_during_stop = true;
    assert(linx_wifi_portal_stop(portal) == ESP_OK);
    assert(shutdown_calls[10] == 1 && shutdown_calls[11] == 1 && shutdown_calls[12] == 0);
    assert(close_calls[10] == 2 && close_calls[11] == 1 && close_calls[12] == 2);
    assert(!sockets_live[10] && !sockets_live[11] && !sockets_live[12]);

    reset();
    portal = start(NETWORKS, 4);
    accept_socket(10, ESP_OK);
    stop_failure = true;
    socket_shutdown_failure = true;
    assert(linx_wifi_portal_stop(portal) == ESP_FAIL && portal->closing);
    assert(shutdown_calls[10] == 1 && sockets_live[10]);
    server_config.close_fn(handlers, 10);
    /* 原 fd 已交给别的模块；后续重试不能 shutdown/close 它。 */
    sockets_live[10] = true;
    sockets_interrupted[10] = false;
    accept_socket(11, ESP_FAIL);
    stop_failure = false;
    assert(linx_wifi_portal_stop(portal) == ESP_OK);
    assert(shutdown_calls[10] == 1 && close_calls[10] == 1 && sockets_live[10]);
    sockets_live[10] = false;

    reset();
    portal = start(NETWORKS, 4);
    accept_socket(10, ESP_OK);
    socket_close_failure = true;
    assert(linx_wifi_portal_stop(portal) == ESP_OK && close_calls[10] == 1);
}

int main(void)
{
    cJSON_Hooks hooks = {.malloc_fn = json_alloc, .free_fn = free};
    cJSON_InitHooks(&hooks);
    test_lifecycle();
    test_page_and_status();
    test_connect();
    test_invalid();
    test_receive_bounds();
    test_socket_cancellation();
    reset();
    assert(mutex_deletes > 0 && stop_calls > 0 && context_releases > 0);
    cJSON_InitHooks(NULL);
    puts("手机网页配网：页面、输入边界、密码清理、状态与资源失败测试通过");
    return 0;
}
