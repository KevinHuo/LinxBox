#include "linx_wifi_portal.h"

#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"

#define REQUEST_CAPACITY 512U
#define STATUS_CAPACITY 2304U
#define RECEIVE_TIMEOUT_US INT64_C(5000000)

struct linx_wifi_portal {
    httpd_handle_t server;
    SemaphoreHandle_t mutex;
    linx_wifi_network_t networks[LINX_WIFI_NETWORK_CAPACITY];
    size_t count;
    linx_wifi_portal_state_t state;
    bool pending;
    bool closing;
    linx_wifi_network_t candidate;
    char password[LINX_WIFI_PASSWORD_CAPACITY];
    int clients[2];
};

static const char PAGE[] =
    "<!doctype html><html lang=\"zh-CN\"><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>LinxBox 设备联网</title><style>"
    ":root{color-scheme:light;font-family:system-ui,-apple-system,sans-serif;color:#352d26;background:#faf7f2}"
    "*{box-sizing:border-box}body{margin:0;padding:36px 20px}main{max-width:440px;margin:auto}"
    ".brand{color:#9c4019;font-size:13px;font-weight:700;letter-spacing:.08em}"
    "h1{font-size:29px;line-height:1.3;margin:18px 0 12px}p{line-height:1.7;color:#71685e;font-size:15px}"
    "form{background:#fff;border:1px solid #e7dfd4;border-radius:20px;padding:24px;margin:26px 0}"
    "label{display:block;font-size:14px;font-weight:600;margin-bottom:9px}"
    "select,input[type=password],input[type=text]{width:100%;min-height:50px;padding:12px;border:1px solid #d8cfc3;"
    "border-radius:10px;background:#fff;color:#352d26;font:inherit;margin-bottom:22px}"
    "input:focus,select:focus,button:focus-visible{outline:3px solid #f5bb91;outline-offset:2px}"
    ".check{display:flex;align-items:center;gap:8px;min-height:44px;margin:-12px 0 12px;font-weight:400}"
    "input[type=checkbox]{width:18px;height:18px;accent-color:#a8471f}"
    "button{width:100%;min-height:50px;border:0;border-radius:11px;background:#a8471f;color:white;font:inherit;font-weight:650}"
    "button:disabled{background:#e7dfd4;color:#71685e}#status{margin:18px 0 0;min-height:26px;font-size:14px}"
    ".note{font-size:13px}code{font:inherit;color:#8b3c1d}"
    "</style><main><div class=\"brand\">LinxBox · 设备联网</div><h1>让设备连接 Wi-Fi</h1>"
    "<p>选择附近的 2.4 GHz 网络，用手机输入密码。连接成功后，配置只保存在设备里。</p>"
    "<form id=\"form\"><label for=\"network\">Wi-Fi 网络</label><select id=\"network\" required disabled></select>"
    "<label for=\"password\">Wi-Fi 密码</label><input id=\"password\" type=\"password\" maxlength=\"64\" autocomplete=\"off\" autocapitalize=\"none\" spellcheck=\"false\">"
    "<label class=\"check\"><input id=\"show\" type=\"checkbox\">显示密码</label>"
    "<button id=\"connect\" disabled>连接并保存</button><p id=\"status\" role=\"status\" aria-live=\"polite\">正在读取附近网络…</p></form>"
    "<p class=\"note\">请保持手机连接设备热点。如果手机提示“无互联网”，选择保留连接。"
    "页面未打开时，在浏览器访问 <code>http://192.168.4.1</code>。配网完成后可切回日常网络。</p></main>"
    "<script>'use strict';const form=document.getElementById('form'),network=document.getElementById('network'),"
    "password=document.getElementById('password'),button=document.getElementById('connect'),status=document.getElementById('status'),"
    "show=document.getElementById('show');let networks=[],loaded=false,busy=false,saved=false;"
    "function controls(){const item=networks[Number(network.value)];network.disabled=busy||saved||!networks.length;"
    "password.disabled=busy||saved||!item||item.security!=='personal';if(password.disabled)password.value='';"
    "button.disabled=busy||saved||!item||item.security==='unsupported';button.textContent=saved?'已保存':busy?'正在连接…':'连接并保存';}"
    "network.addEventListener('change',()=>{password.value='';controls();});show.addEventListener('change',()=>{password.type=show.checked?'text':'password';});"
    "async function update(){try{const response=await fetch('/api/status',{cache:'no-store'});if(!response.ok)throw Error();const data=await response.json();"
    "if(!loaded){networks=data.networks;network.replaceChildren();networks.forEach((item,index)=>{const option=document.createElement('option');"
    "option.value=String(index);option.text=item.ssid+(item.security==='open'?'（无需密码）':item.security==='unsupported'?'（暂不支持）':'');"
    "option.disabled=item.security==='unsupported';network.add(option);});const first=networks.findIndex(item=>item.security!=='unsupported');"
    "if(first>=0)network.value=String(first);loaded=true;}busy=data.state==='connecting';saved=data.state==='saved';"
    "status.textContent=saved?'连接成功，已保存。可以返回设备继续使用。':busy?'正在尝试连接，请保持本页打开…':"
    "data.state==='error'?'连接失败，请检查密码或网络后重试。':networks.some(item=>item.security!=='unsupported')?'请选择网络并输入密码。':'未找到可用网络，请在设备上退出配网后重试。';controls();"
    "}catch(error){status.textContent='暂时无法连接设备，请确认手机仍连接设备热点。';}}"
    "form.addEventListener('submit',async event=>{event.preventDefault();const item=networks[Number(network.value)];if(!item||busy||saved)return;"
    "let body=JSON.stringify({ssid:item.ssid,password:password.value});password.value='';show.checked=false;password.type='password';busy=true;controls();"
    "status.textContent='正在发送配置…';try{const response=await fetch('/api/connect',{method:'POST',headers:{'Content-Type':'application/json'},body});"
    "if(!response.ok){busy=false;controls();status.textContent=response.status===409?'设备正在连接，请稍候。':'请检查网络和密码后重新输入。';}else await update();"
    "}catch(error){busy=false;controls();status.textContent='发送失败，请保持手机连接设备热点后重试。';}finally{body='';}});"
    "update();setInterval(update,2000);</script></html>";

static void clear_secret(void *memory, size_t size)
{
    volatile unsigned char *bytes = memory;
    while (size-- > 0U) *bytes++ = 0;
}

static bool lock(linx_wifi_portal_t *portal)
{
    return xSemaphoreTake(portal->mutex, portMAX_DELAY) == pdTRUE;
}

static void keep_context(void *context)
{
    /* HTTPD 拥有回调时序，实例仅由工作任务在成功停止后释放。 */
    (void)context;
}

static esp_err_t socket_open(httpd_handle_t server, int socket)
{
    linx_wifi_portal_t *portal = httpd_get_global_user_ctx(server);
    esp_err_t error = ESP_FAIL;
    xSemaphoreTake(portal->mutex, portMAX_DELAY);
    if (!portal->closing) {
        for (size_t i = 0; i < sizeof(portal->clients) / sizeof(portal->clients[0]); ++i) {
            if (portal->clients[i] < 0) {
                portal->clients[i] = socket;
                error = ESP_OK;
                break;
            }
        }
    }
    xSemaphoreGive(portal->mutex);
    return error;
}

static void socket_close(httpd_handle_t server, int socket)
{
    linx_wifi_portal_t *portal = httpd_get_global_user_ctx(server);
    xSemaphoreTake(portal->mutex, portMAX_DELAY);
    for (size_t i = 0; i < sizeof(portal->clients) / sizeof(portal->clients[0]); ++i) {
        if (portal->clients[i] == socket) {
            portal->clients[i] = -1;
            /* 同锁移除并关闭，防止 stop 误操作已被复用的 fd。 */
            close(socket);
            break;
        }
    }
    /* open 拒绝的 fd 未被记录，须交由 SDK accept 尾部关闭。 */
    xSemaphoreGive(portal->mutex);
}

static esp_err_t reply(httpd_req_t *request, const char *status, const char *type,
                       const char *body)
{
    esp_err_t error = httpd_resp_set_status(request, status);
    if (error == ESP_OK) error = httpd_resp_set_type(request, type);
    if (error == ESP_OK) error = httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    if (error == ESP_OK) error = httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    if (error == ESP_OK) error = httpd_resp_set_hdr(request, "Connection", "close");
    if (error == ESP_OK) error = httpd_resp_send(request, body, HTTPD_RESP_USE_STRLEN);
    return error;
}

static esp_err_t page_get(httpd_req_t *request)
{
    if (request->content_len > 0U) {
        reply(request, "400 Bad Request", "application/json", "{}");
        return ESP_FAIL;
    }
    return reply(request, "200 OK", "text/html; charset=utf-8", PAGE);
}

static esp_err_t status_get(httpd_req_t *request)
{
    static const char *const states[] = {"ready", "connecting", "saved", "error"};
    static const char *const securities[] = {"open", "personal", "unsupported"};
    linx_wifi_portal_t *portal = request->user_ctx;
    if (request->content_len > 0U) {
        reply(request, "400 Bad Request", "application/json", "{}");
        return ESP_FAIL;
    }
    if (!lock(portal)) return ESP_FAIL;
    linx_wifi_portal_state_t state = portal->state;
    bool closing = portal->closing;
    xSemaphoreGive(portal->mutex);
    if (closing) return reply(request, "503 Service Unavailable", "application/json", "{}");
    cJSON *root = cJSON_CreateObject();
    cJSON *networks = cJSON_AddArrayToObject(root, "networks");
    bool valid = cJSON_AddStringToObject(root, "state", states[state]) != NULL && networks != NULL;
    for (size_t i = 0; valid && i < portal->count; ++i) {
        const linx_wifi_network_t *network = &portal->networks[i];
        cJSON *item = cJSON_CreateObject();
        if (item == NULL) { valid = false; break; }
        valid = cJSON_AddStringToObject(item, "ssid", network->ssid) != NULL &&
                cJSON_AddStringToObject(item, "security", securities[network->security]) != NULL &&
                cJSON_AddNumberToObject(item, "rssi", network->rssi) != NULL;
        cJSON_AddItemToArray(networks, item);
    }
    char body[STATUS_CAPACITY];
    valid = valid && cJSON_PrintPreallocated(root, body, sizeof(body), false);
    cJSON_Delete(root);
    return valid ? reply(request, "200 OK", "application/json", body)
                 : reply(request, "503 Service Unavailable", "application/json", "{}");
}

static void whitespace(const char **cursor)
{
    while (**cursor == ' ' || **cursor == '\t' || **cursor == '\r' || **cursor == '\n') ++*cursor;
}

/* 只接受两个字符串字段，不让密码经过会在失败时释放未擦除副本的 JSON 堆。 */
static bool string_token(const char **cursor, const char **start, size_t *length)
{
    if (**cursor != '"') return false;
    *start = (*cursor)++;
    while (**cursor != '\0' && **cursor != '"') {
        unsigned char character = (unsigned char)*(*cursor)++;
        if (character < 32U) return false;
        if (character == '\\') {
            if (**cursor == '\0') return false;
            if (strncmp(*cursor, "u0000", 5) == 0) return false;
            ++*cursor;
        }
    }
    if (**cursor != '"') return false;
    ++*cursor;
    *length = (size_t)(*cursor - *start);
    return true;
}

static int hex_digit(char character)
{
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'a' && character <= 'f') return character - 'a' + 10;
    if (character >= 'A' && character <= 'F') return character - 'A' + 10;
    return -1;
}

static bool ascii_string(const char *start, size_t length, char *out, size_t capacity)
{
    size_t used = 0;
    for (size_t i = 1; i < length - 1U; ++i) {
        unsigned character = (unsigned char)start[i];
        if (character == '\\') {
            character = (unsigned char)start[++i];
            if (character == 'u') {
                if (i + 4U >= length - 1U) return false;
                character = 0;
                for (size_t digit = 0; digit < 4U; ++digit) {
                    int value = hex_digit(start[++i]);
                    if (value < 0) return false;
                    character = (character << 4U) | (unsigned)value;
                }
            } else if (character != '"' && character != '\\' && character != '/') return false;
        }
        if (character < 32U || character > 126U || used + 1U >= capacity) return false;
        out[used++] = (char)character;
    }
    out[used] = '\0';
    return true;
}

static bool credentials(const char *body, char ssid[LINX_WIFI_SSID_CAPACITY],
                        char password[LINX_WIFI_PASSWORD_CAPACITY])
{
    const char *cursor = body;
    bool has_ssid = false, has_password = false;
    whitespace(&cursor);
    if (*cursor++ != '{') return false;
    for (size_t i = 0; i < 2U; ++i) {
        const char *start;
        size_t length;
        char key[9];
        whitespace(&cursor);
        if (!string_token(&cursor, &start, &length) || !ascii_string(start, length, key, sizeof(key))) return false;
        whitespace(&cursor);
        if (*cursor++ != ':') return false;
        whitespace(&cursor);
        if (!string_token(&cursor, &start, &length)) return false;
        if (strcmp(key, "ssid") == 0 && !has_ssid) {
            const char *end = NULL;
            cJSON *value = cJSON_ParseWithLengthOpts(start, length, &end, false);
            bool valid = cJSON_IsString(value) && end == start + length &&
                         strlen(value->valuestring) > 0U && strlen(value->valuestring) < LINX_WIFI_SSID_CAPACITY;
            if (valid) strcpy(ssid, value->valuestring);
            cJSON_Delete(value);
            if (!valid) return false;
            has_ssid = true;
        } else if (strcmp(key, "password") == 0 && !has_password) {
            if (!ascii_string(start, length, password, LINX_WIFI_PASSWORD_CAPACITY)) return false;
            has_password = true;
        } else return false;
        whitespace(&cursor);
        if (*cursor++ != (i == 0U ? ',' : '}')) return false;
    }
    whitespace(&cursor);
    return *cursor == '\0' && has_ssid && has_password;
}

static bool header_is(httpd_req_t *request, const char *name, const char *expected,
                      bool optional)
{
    size_t length = httpd_req_get_hdr_value_len(request, name);
    if (length == 0U) return optional;
    char value[64];
    return length < sizeof(value) &&
           httpd_req_get_hdr_value_str(request, name, value, sizeof(value)) == ESP_OK &&
           strcmp(value, expected) == 0;
}

static const char *receive_status(linx_wifi_portal_t *portal, int64_t started)
{
    if (!lock(portal)) return "503 Service Unavailable";
    bool closing = portal->closing;
    xSemaphoreGive(portal->mutex);
    if (closing) return "503 Service Unavailable";
    return esp_timer_get_time() - started >= RECEIVE_TIMEOUT_US ? "408 Request Timeout" : NULL;
}

static esp_err_t connect_post(httpd_req_t *request)
{
    linx_wifi_portal_t *portal = request->user_ctx;
    const char *status = "400 Bad Request";
    char body[REQUEST_CAPACITY + 1U] = {0};
    char ssid[LINX_WIFI_SSID_CAPACITY] = {0};
    char password[LINX_WIFI_PASSWORD_CAPACITY] = {0};
    size_t received = 0;
    if (!header_is(request, "Origin", "http://192.168.4.1", true)) {
        status = "403 Forbidden";
    } else if (!header_is(request, "Content-Type", "application/json", false)) {
        status = "415 Unsupported Media Type";
    } else if (request->content_len > REQUEST_CAPACITY) {
        status = "413 Content Too Large";
    } else {
        int64_t started = esp_timer_get_time();
        const char *failure = NULL;
        while (received < request->content_len) {
            failure = receive_status(portal, started);
            if (failure != NULL) break;
            int size = httpd_req_recv(request, body + received, request->content_len - received);
            if (size > 0) received += (size_t)size;
            failure = receive_status(portal, started);
            if (failure != NULL) break;
            if (size <= 0) break;
        }
        if (failure != NULL) status = failure;
        if (failure == NULL && received == request->content_len && memchr(body, '\0', received) == NULL &&
            credentials(body, ssid, password)) {
            for (size_t i = 0; i < portal->count; ++i) {
                const linx_wifi_network_t *network = &portal->networks[i];
                if (strcmp(ssid, network->ssid) != 0 || network->security == LINX_WIFI_UNSUPPORTED ||
                    !linx_wifi_password_valid(password, network->security != LINX_WIFI_OPEN)) continue;
                if (!lock(portal)) break;
                if (portal->closing) status = "503 Service Unavailable";
                else if (portal->pending || portal->state == LINX_WIFI_PORTAL_CONNECTING ||
                         portal->state == LINX_WIFI_PORTAL_SAVED) status = "409 Conflict";
                else {
                    portal->candidate = *network;
                    memcpy(portal->password, password, sizeof(password));
                    portal->pending = true;
                    portal->state = LINX_WIFI_PORTAL_CONNECTING;
                    status = "202 Accepted";
                }
                xSemaphoreGive(portal->mutex);
                break;
            }
        }
    }
    clear_secret(password, sizeof(password));
    clear_secret(body, sizeof(body));
    esp_err_t error = reply(request, status, "application/json", "{}");
    /* 非成功返回让 HTTPD 直接关闭会话，避免框架继续无界排空慢请求体。 */
    return received < request->content_len ? ESP_FAIL : error;
}

esp_err_t linx_wifi_portal_start(linx_wifi_portal_t **out,
                               const linx_wifi_network_t *networks, size_t count)
{
    if (out == NULL || count > LINX_WIFI_NETWORK_CAPACITY || (count > 0U && networks == NULL)) return ESP_FAIL;
    *out = NULL;
    for (size_t i = 0; i < count; ++i) {
        if (networks[i].ssid[0] == '\0' || memchr(networks[i].ssid, '\0', LINX_WIFI_SSID_CAPACITY) == NULL ||
            networks[i].security < LINX_WIFI_OPEN || networks[i].security > LINX_WIFI_UNSUPPORTED) return ESP_FAIL;
    }
    linx_wifi_portal_t *portal = calloc(1, sizeof(*portal));
    if (portal == NULL) return ESP_ERR_NO_MEM;
    portal->mutex = xSemaphoreCreateMutex();
    if (portal->mutex == NULL) { free(portal); return ESP_ERR_NO_MEM; }
    for (size_t i = 0; i < sizeof(portal->clients) / sizeof(portal->clients[0]); ++i) portal->clients[i] = -1;
    if (count > 0U) memcpy(portal->networks, networks, count * sizeof(*networks));
    portal->count = count;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 6144;
    config.max_open_sockets = 2;
    config.max_uri_handlers = 3;
    config.max_resp_headers = 3;
    config.recv_wait_timeout = 2;
    config.send_wait_timeout = 2;
    config.lru_purge_enable = true;
    config.global_user_ctx = portal;
    config.global_user_ctx_free_fn = keep_context;
    config.open_fn = socket_open;
    config.close_fn = socket_close;
    esp_err_t error = httpd_start(&portal->server, &config);
    const httpd_uri_t handlers[] = {
        {.uri = "/", .method = HTTP_GET, .handler = page_get, .user_ctx = portal},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_get, .user_ctx = portal},
        {.uri = "/api/connect", .method = HTTP_POST, .handler = connect_post, .user_ctx = portal},
    };
    for (size_t i = 0; error == ESP_OK && i < sizeof(handlers) / sizeof(handlers[0]); ++i)
        error = httpd_register_uri_handler(portal->server, &handlers[i]);
    if (error != ESP_OK) {
        /* 若停止失败则仍交回实例，让同一设备任务重试清理。 */
        if (linx_wifi_portal_stop(portal) != ESP_OK) *out = portal;
        return error;
    }
    *out = portal;
    return ESP_OK;
}

bool linx_wifi_portal_poll(linx_wifi_portal_t *portal, linx_wifi_network_t *network,
                          char password[LINX_WIFI_PASSWORD_CAPACITY])
{
    if (portal == NULL || network == NULL || password == NULL) return false;
    clear_secret(password, LINX_WIFI_PASSWORD_CAPACITY);
    if (!lock(portal)) return false;
    bool pending = portal->pending && !portal->closing;
    if (pending) {
        *network = portal->candidate;
        memcpy(password, portal->password, LINX_WIFI_PASSWORD_CAPACITY);
        clear_secret(portal->password, sizeof(portal->password));
        portal->pending = false;
    }
    xSemaphoreGive(portal->mutex);
    return pending;
}

void linx_wifi_portal_status(linx_wifi_portal_t *portal, linx_wifi_portal_state_t state)
{
    if (portal == NULL || state < LINX_WIFI_PORTAL_READY || state > LINX_WIFI_PORTAL_ERROR || !lock(portal)) return;
    if (!portal->closing) portal->state = state;
    xSemaphoreGive(portal->mutex);
}

esp_err_t linx_wifi_portal_stop(linx_wifi_portal_t *portal)
{
    if (portal == NULL) return ESP_OK;
    if (!lock(portal)) return ESP_FAIL;
    portal->closing = true;
    portal->pending = false;
    clear_secret(portal->password, sizeof(portal->password));
    for (size_t i = 0; i < sizeof(portal->clients) / sizeof(portal->clients[0]); ++i) {
        if (portal->clients[i] >= 0) shutdown(portal->clients[i], SHUT_RDWR);
    }
    xSemaphoreGive(portal->mutex);
    if (portal->server != NULL) {
        esp_err_t error = httpd_stop(portal->server);
        if (error != ESP_OK) return error;
    }
    vSemaphoreDelete(portal->mutex);
    clear_secret(portal, sizeof(*portal));
    free(portal);
    return ESP_OK;
}
