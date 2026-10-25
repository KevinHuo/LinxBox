#include "linx_gateway_client.h"
#include "linx_gateway_contract.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"

_Static_assert(LINX_GATEWAY_SESSION_MAX <= LINX_GATEWAY_CONTRACT_SESSION_MAX, "Schema 会话容量发生变化");
_Static_assert(LINX_GATEWAY_HISTORY_MAX <= LINX_GATEWAY_CONTRACT_HISTORY_MAX, "Schema 历史容量发生变化");

enum { OP_NONE, OP_HELLO, OP_SESSIONS, OP_DETAIL, OP_HISTORY, OP_CONTENT,
       OP_PREPARE, OP_CONFIRM, OP_CANCEL, OP_GET };
static const char *const operations[] = { "", "device.hello", "agent_sessions.list",
    "agent_session.get", "messages.list", "message.content.get", "request.prepare",
    "request.confirm", "request.cancel", "request.get" };
static const char *const request_statuses[] = { "", "awaiting_text", "awaiting_confirmation",
    "sending", "running", "completed", "failed", "cancelled", "unknown" };

static bool utf8(const char *s, size_t n, size_t *points)
{
    size_t count = 0;
    for (size_t i = 0; i < n;) {
        unsigned char first = (unsigned char)s[i++];
        unsigned value = first;
        size_t extra = 0;
        if (first >= 0xc2 && first <= 0xdf) { value &= 31U; extra = 1; }
        else if (first >= 0xe0 && first <= 0xef) { value &= 15U; extra = 2; }
        else if (first >= 0xf0 && first <= 0xf4) { value &= 7U; extra = 3; }
        else if (first >= 0x80 || first == 0) { return false; }
        if (extra > n - i) { return false; }
        for (size_t j = 0; j < extra; ++j) {
            unsigned char next = (unsigned char)s[i++];
            if ((next & 0xc0U) != 0x80U) { return false; }
            value = (value << 6U) | (next & 63U);
        }
        if ((extra == 2 && value < 0x800) || (extra == 3 && value < 0x10000) ||
            value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) { return false; }
        ++count;
    }
    if (points != NULL) { *points = count; }
    return true;
}

static int hex(char ch)
{
    if (ch >= '0' && ch <= '9') { return ch - '0'; }
    if (ch >= 'a' && ch <= 'f') { return ch - 'a' + 10; }
    if (ch >= 'A' && ch <= 'F') { return ch - 'A' + 10; }
    return -1;
}

static bool unicode_escape(const char *s, size_t n, size_t *position, unsigned *value)
{
    if (n - *position < 4) { return false; }
    *value = 0;
    for (unsigned j = 0; j < 4; ++j) {
        int digit = hex(s[(*position)++]);
        if (digit < 0) { return false; }
        *value = (*value << 4U) | (unsigned)digit;
    }
    return *value != 0;
}

static bool scan_string(const char *s, size_t n, size_t *position)
{
    size_t start = ++*position;
    while (*position < n) {
        unsigned char ch = (unsigned char)s[*position];
        if (ch == '"' || ch == '\\') {
            if (!utf8(s + start, *position - start, NULL)) { return false; }
            ++*position;
            if (ch == '"') { return true; }
            if (*position >= n) { return false; }
            ch = (unsigned char)s[(*position)++];
            if (ch == 'u') {
                unsigned value;
                if (!unicode_escape(s, n, position, &value)) { return false; }
                if (value >= 0xd800 && value <= 0xdbff) {
                    if (n - *position < 6 || s[*position] != '\\' || s[*position + 1] != 'u') { return false; }
                    *position += 2;
                    if (!unicode_escape(s, n, position, &value) || value < 0xdc00 || value > 0xdfff) { return false; }
                } else if (value >= 0xdc00 && value <= 0xdfff) { return false; }
            } else if (strchr("\"\\/bfnrt", ch) == NULL) { return false; }
            start = *position;
        } else {
            if (ch < 0x20) { return false; }
            ++*position;
        }
    }
    return false;
}

/* 不递归；在 cJSON 分配任何节点前限制输入树与字符串。 */
static bool preflight(const char *s, size_t n)
{
    if (s == NULL || n == 0 || n > LINX_GATEWAY_FRAME_MAX) { return false; }
    unsigned char stack[9] = {0};
    size_t depth = 0, nodes = 0, i = 0;
    while (i < n) {
        char ch = s[i];
        if (ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t') { ++i; continue; }
        unsigned state = stack[depth];
        if (state == 1) { return false; }
        if (state == 2 || state == 3) {
            if (ch == '}' && state == 2) { --depth; ++i; continue; }
            if (ch != '"' || !scan_string(s, n, &i)) { return false; }
            stack[depth] = 4;
            continue;
        }
        if (state == 4) {
            if (ch != ':') { return false; }
            stack[depth] = 5; ++i; continue;
        }
        if (state == 6 || state == 8) {
            if ((state == 6 && ch == '}') || (state == 8 && ch == ']')) { --depth; ++i; continue; }
            if (ch != ',') { return false; }
            stack[depth] = state == 6 ? 3 : 9; ++i; continue;
        }
        if (state == 7 && ch == ']') { --depth; ++i; continue; }
        if (++nodes > 256) { return false; }
        stack[depth] = depth == 0 ? 1 : (state == 5 ? 6 : 8);
        if (ch == '{' || ch == '[') {
            if (++depth > 8) { return false; }
            stack[depth] = ch == '{' ? 2 : 7; ++i;
        } else if (ch == '"') {
            if (!scan_string(s, n, &i)) { return false; }
        } else if (ch == 't' || ch == 'f' || ch == 'n') {
            const char *literal = ch == 't' ? "true" : ch == 'f' ? "false" : "null";
            size_t length = strlen(literal);
            if (length > n - i || memcmp(s + i, literal, length) != 0) { return false; }
            i += length;
        } else {
            if (ch == '-') { if (++i == n) { return false; } ch = s[i]; }
            if (ch == '0') { ++i; }
            else if (ch >= '1' && ch <= '9') { do { ++i; } while (i < n && s[i] >= '0' && s[i] <= '9'); }
            else { return false; }
            if (i < n && s[i] == '.') {
                ++i; size_t digits = i;
                while (i < n && s[i] >= '0' && s[i] <= '9') { ++i; }
                if (digits == i) { return false; }
            }
            if (i < n && (s[i] == 'e' || s[i] == 'E')) {
                ++i;
                if (i < n && (s[i] == '+' || s[i] == '-')) { ++i; }
                size_t digits = i;
                while (i < n && s[i] >= '0' && s[i] <= '9') { ++i; }
                if (digits == i) { return false; }
            }
        }
    }
    return depth == 0 && stack[0] == 1;
}

static bool mac_valid(const char *s, bool canonical)
{
    size_t n = strlen(s);
    if (canonical && n != 17) { return false; }
    if (n != 12 && n != 17) { return false; }
    for (size_t i = 0; i < n; ++i) {
        if (n == 17 && i % 3 == 2) {
            if ((s[i] != ':' && s[i] != '-') || (canonical && s[i] != ':') || s[i] != s[2]) { return false; }
        } else if (hex(s[i]) < 0 || (canonical && s[i] >= 'a' && s[i] <= 'f')) { return false; }
    }
    return true;
}

static bool matches_pattern(unsigned pattern, const char *s)
{
    if (pattern == 1 || pattern == 2) { return mac_valid(s, pattern == 2); }
    if (pattern == 3) {
        while (*s) {
            unsigned char ch = (unsigned char)*s++;
            unsigned value = ch;
            unsigned extra = ch < 0x80 ? 0 : ch < 0xe0 ? 1 : ch < 0xf0 ? 2 : 3;
            if (extra) { value &= (1U << (6U - extra)) - 1U; }
            for (unsigned i = 0; i < extra; ++i) { value = (value << 6U) | ((unsigned char)*s++ & 63U); }
            bool space = (value >= 9 && value <= 13) || (value >= 0x1c && value <= 0x20) ||
                value == 0x85 || value == 0xa0 || value == 0x1680 || (value >= 0x2000 && value <= 0x200a) ||
                value == 0x2028 || value == 0x2029 || value == 0x202f || value == 0x205f || value == 0x3000;
            if (!space) { return true; }
        }
        return false;
    }
    return true;
}

static bool schema_valid(unsigned index, const cJSON *value)
{
    const linx_contract_node_t *node = &linx_contract_nodes[index];
    if (node->ref >= 0 && !schema_valid((unsigned)node->ref, value)) { return false; }
    unsigned actual = cJSON_IsString(value) ? 1U : cJSON_IsObject(value) ? 2U :
        cJSON_IsArray(value) ? 4U : cJSON_IsNumber(value) ? 8U : cJSON_IsNull(value) ? 16U :
        cJSON_IsBool(value) ? 32U : 0;
    if (node->types && !(node->types & actual)) { return false; }
    if (node->const_kind == 1 && (!cJSON_IsString(value) || strcmp(value->valuestring, node->constant))) { return false; }
    if (node->const_kind == 2 && (!cJSON_IsBool(value) || cJSON_IsTrue(value) != node->boolean)) { return false; }
    if (cJSON_IsString(value)) {
        size_t points;
        if (!utf8(value->valuestring, strlen(value->valuestring), &points) || points < node->min_length ||
            (node->max_length && points > node->max_length) || !matches_pattern(node->pattern, value->valuestring)) { return false; }
        if (node->enum_count) {
            bool found = false;
            for (unsigned i = 0; i < node->enum_count; ++i) {
                if (!strcmp(value->valuestring, linx_contract_values[node->enum_start + i])) { found = true; }
            }
            if (!found) { return false; }
        }
    }
    if (cJSON_IsNumber(value) && node->types == 8 && (!isfinite(value->valuedouble) ||
        value->valuedouble < node->minimum || floor(value->valuedouble) != value->valuedouble ||
        (node->maximum_set && value->valuedouble > node->maximum) || value->valuedouble > 9007199254740991.0)) { return false; }
    if (cJSON_IsObject(value)) {
        for (const cJSON *item = value->child; item; item = item->next) {
            bool found = node->additional != 0;
            for (const cJSON *old = value->child; old != item; old = old->next) {
                if (!strcmp(old->string, item->string)) { return false; }
            }
            for (unsigned i = 0; i < node->prop_count; ++i) {
                const linx_contract_property_t *property = &linx_contract_properties[node->prop_start + i];
                if (!strcmp(property->name, item->string)) {
                    if (!schema_valid(property->node, item)) { return false; }
                    found = true;
                }
            }
            if (!found) { return false; }
        }
        for (unsigned i = 0; i < node->prop_count; ++i) {
            const linx_contract_property_t *property = &linx_contract_properties[node->prop_start + i];
            if (property->required && !cJSON_GetObjectItemCaseSensitive(value, property->name)) { return false; }
        }
    }
    if (cJSON_IsArray(value)) {
        if (node->max_items && (unsigned)cJSON_GetArraySize(value) > node->max_items) { return false; }
        if (node->item >= 0) {
            for (const cJSON *item = value->child; item; item = item->next) {
                if (!schema_valid((unsigned)node->item, item)) { return false; }
            }
        }
    }
    unsigned one = 0, any = 0;
    for (unsigned i = 0; i < node->one_count; ++i) { one += schema_valid(linx_contract_links[node->one_start + i], value); }
    for (unsigned i = 0; i < node->any_count; ++i) { any += schema_valid(linx_contract_links[node->any_start + i], value); }
    if ((node->one_count && one != 1) || (node->any_count && !any)) { return false; }
    for (unsigned i = 0; i < node->all_count; ++i) {
        if (!schema_valid(linx_contract_links[node->all_start + i], value)) { return false; }
    }
    if (node->condition >= 0 && schema_valid((unsigned)node->condition, value) &&
        node->consequence >= 0 && !schema_valid((unsigned)node->consequence, value)) { return false; }
    return true;
}

static bool definition_valid(const char *definition, const cJSON *value)
{
    for (size_t i = 0; i < sizeof(linx_contract_definitions) / sizeof(linx_contract_definitions[0]); ++i) {
        if (!strcmp(definition, linx_contract_definitions[i].name)) { return schema_valid(linx_contract_definitions[i].node, value); }
    }
    return false;
}

static cJSON *parse(const char *bytes, size_t size)
{
    if (!preflight(bytes, size)) { return NULL; }
    const char *end = NULL, *limit = bytes + size;
    cJSON *root = cJSON_ParseWithLengthOpts(bytes, size, &end, false);
    if (!root) return NULL;
    /* 借出帧不要求NUL；只接受JSON四种尾白，不能读取size之外的哨兵。 */
    while (end < limit && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) ++end;
    if (end != limit) { cJSON_Delete(root); return NULL; }
    return root;
}

bool linx_gateway_client_validate_json(const char *definition, const char *bytes, size_t size)
{
    if (!definition) { return false; }
    cJSON *root = parse(bytes, size);
    bool valid = root && definition_valid(definition, root);
    cJSON_Delete(root);
    return valid;
}

static const cJSON *field(const cJSON *object, const char *name)
{
    return cJSON_GetObjectItemCaseSensitive(object, name);
}

static const char *string(const cJSON *object, const char *name)
{
    const cJSON *value = field(object, name);
    return cJSON_IsString(value) ? value->valuestring : "";
}

static const char *known_value(const char *value, const char *const *allowed, size_t count)
{
    for (size_t i = 0; i < count; ++i) if (!strcmp(value, allowed[i])) return allowed[i];
    return "";
}

static void protocol_log(const linx_gateway_client_t *client, uint32_t generation, const char *phase,
                         const char *type, unsigned operation, size_t size, const cJSON *data, const cJSON *error)
{
    if (!client->config.log) return;
    static const char *const notices[] = {"response", "device.bound", "request.proposed", "request.updated",
        "agent_sessions.updated", "message.available"};
    static const char *const codes[] = {"INVALID_ARGUMENT", "UNAUTHENTICATED", "PERMISSION_DENIED", "NOT_FOUND",
        "CONFLICT", "DEPENDENCY_UNAVAILABLE", "RESOURCE_EXHAUSTED", "INTERNAL"};
    const char *message_type = known_value(type, operations, sizeof(operations) / sizeof(operations[0]));
    if (!*message_type) message_type = known_value(type, notices, sizeof(notices) / sizeof(notices[0]));
    const linx_gateway_log_t entry = {
        .phase = phase, .type = message_type, .operation = operations[operation],
        .status = known_value(string(data, "status"), request_statuses, sizeof(request_statuses) / sizeof(request_statuses[0])),
        .error_code = known_value(string(error, "code"), codes, sizeof(codes) / sizeof(codes[0])),
        .generation = generation, .size = size,
    };
    client->config.log(&entry, client->config.user);
}

static void copy_string(char *out, const char *in) { memmove(out, in, strlen(in) + 1); }

static bool fits(const cJSON *object, const char *name, size_t capacity)
{
    return strlen(string(object, name)) < capacity;
}

static void changed(linx_gateway_client_t *client) { ++client->view.revision; }

static bool fail(linx_gateway_client_t *client, linx_gateway_error_t error, const char *message)
{
    client->view.error = error;
    copy_string(client->view.error_message, message);
    client->view.error_code[0] = '\0';
    changed(client);
    return false;
}

static void clear_error(linx_gateway_client_t *client)
{
    client->view.error = LINX_GATEWAY_ERROR_NONE;
    client->view.error_code[0] = '\0'; client->view.error_message[0] = '\0';
}

static bool terminal(linx_gateway_request_status_t status)
{
    return status == LINX_GATEWAY_REQUEST_COMPLETED || status == LINX_GATEWAY_REQUEST_FAILED || status == LINX_GATEWAY_REQUEST_CANCELLED;
}

static bool id_valid(const char *id)
{
    size_t length = id ? strlen(id) : 0, points = 0;
    return length > 0 && length < LINX_GATEWAY_ID_CAPACITY && utf8(id, length, &points) && points <= LINX_GATEWAY_CONTRACT_ID_POINTS;
}

static bool session_fits(const cJSON *value)
{
    return fits(value, "title", LINX_GATEWAY_TITLE_CAPACITY) && fits(value, "project_name", LINX_GATEWAY_PROJECT_CAPACITY) &&
        fits(value, "agent", LINX_GATEWAY_AGENT_CAPACITY);
}

static void session_copy(linx_gateway_session_t *out, const cJSON *value)
{
    copy_string(out->agent_session_id, string(value, "agent_session_id"));
    copy_string(out->title, string(value, "title"));
    copy_string(out->project_name, string(value, "project_name"));
    copy_string(out->agent, string(value, "agent"));
    copy_string(out->status, string(value, "status"));
}

static bool duplicates(const cJSON *items, const char *key)
{
    for (const cJSON *item = items->child; item; item = item->next) {
        for (const cJSON *old = items->child; old != item; old = old->next) {
            if (!strcmp(string(old, key), string(item, key))) { return true; }
        }
    }
    return false;
}

static bool make_id(linx_gateway_client_t *client, const char *kind, char *out, size_t capacity)
{
    if (client->sequence == UINT32_MAX) { return fail(client, LINX_GATEWAY_ERROR_CAPACITY, "本次启动请求编号已用尽"); }
    ++client->sequence;
    snprintf(out, capacity, "linx-%c-%016llx-%s-%08lx-%08lx", kind[0],
             (unsigned long long)client->config.instance_id, client->view.device_id,
             (unsigned long)client->view.generation, (unsigned long)client->sequence);
    return true;
}

static void pending_clear(linx_gateway_client_t *client)
{
    client->pending.operation = OP_NONE; client->view.loading = false;
}

static bool send_operation(linx_gateway_client_t *client, unsigned operation, const char *session,
                            const char *message, const char *cursor, uint32_t offset, uint32_t now)
{
    char id[64]; if (!make_id(client, "packet", id, sizeof(id))) { return false; }
    cJSON *root = cJSON_CreateObject(), *payload = cJSON_CreateObject();
    if (!root || !payload) { cJSON_Delete(root); cJSON_Delete(payload); return fail(client, LINX_GATEWAY_ERROR_MEMORY, "消息内存不足"); }
    bool ok = cJSON_AddStringToObject(root, "id", id) && cJSON_AddStringToObject(root, "type", operations[operation]);
    if (operation == OP_HELLO) { ok = ok && cJSON_AddStringToObject(payload, "mac_address", client->view.device_id); }
    if (operation == OP_DETAIL || operation == OP_HISTORY || operation == OP_CONTENT || operation == OP_PREPARE) {
        ok = ok && cJSON_AddStringToObject(payload, "agent_session_id", session);
    }
    if (operation == OP_CONTENT) {
        ok = ok && cJSON_AddStringToObject(payload, "message_id", message) && cJSON_AddNumberToObject(payload, "offset", offset);
    }
    if (operation == OP_SESSIONS || operation == OP_HISTORY) {
        if (*cursor) { ok = ok && cJSON_AddStringToObject(payload, "cursor", cursor); }
    }
    if (operation >= OP_PREPARE) { ok = ok && cJSON_AddStringToObject(payload, "request_id", client->view.request.request_id); }
    if (!cJSON_AddItemToObject(root, "payload", payload)) { cJSON_Delete(payload); ok = false; }
    char *json = ok ? cJSON_PrintUnformatted(root) : NULL;
    if (!json) { cJSON_Delete(root); return fail(client, LINX_GATEWAY_ERROR_MEMORY, "消息内存不足"); }
    size_t size = strlen(json);
    bool accepted = client->config.send && client->config.send(client->view.generation, json, size, client->config.user);
    protocol_log(client, client->view.generation, accepted ? "tx_queued" : "tx_rejected", operations[operation],
                 operation, size, NULL, NULL);
    cJSON_free(json); cJSON_Delete(root);
    if (!accepted) { return fail(client, LINX_GATEWAY_ERROR_BUSY, "发送队列繁忙，请重试"); }
    client->pending.operation = operation;
    copy_string(client->pending.id, id);
    copy_string(client->pending.session, session);
    copy_string(client->pending.message, message);
    copy_string(client->pending.cursor, cursor);
    copy_string(client->pending.request, client->view.request.request_id);
    client->pending.offset = offset; client->pending.sent_at = now; client->pending.generation = client->view.generation;
    client->view.loading = true;
    if (operation == OP_PREPARE) { client->prepare_sent = true; }
    if (operation == OP_CONFIRM) { client->view.request.confirm_sent = true; }
    if (operation == OP_CANCEL) { client->view.request.cancel_sent = true; }
    changed(client);
    return true;
}

void linx_gateway_client_init(linx_gateway_client_t *client, const linx_gateway_client_config_t *config)
{
    if (!client) { return; }
    memset(client, 0, sizeof(*client));
    if (config) { client->config = *config; }
    client->view.configured = config && config->configured && config->instance_id != 0;
    if (!client->view.configured) { fail(client, LINX_GATEWAY_ERROR_UNCONFIGURED, "服务未配置"); }
}

void linx_gateway_client_deinit(linx_gateway_client_t *client)
{
    if (!client) return;
    memset(client, 0, sizeof(*client));
}

void linx_gateway_client_connected(linx_gateway_client_t *client, uint32_t generation, const char *mac, uint32_t now)
{
    if (!client || !client->view.configured) { return; }
    if (!mac || !mac_valid(mac, false)) { fail(client, LINX_GATEWAY_ERROR_INVALID, "设备 MAC 无效"); return; }
    size_t source = 0;
    for (size_t i = 0; i < 17; ++i) {
        client->view.device_id[i] = i % 3 == 2 ? ':' : (char)toupper((unsigned char)mac[source++]);
        if (i % 3 == 2 && strlen(mac) == 17) { ++source; }
    }
    client->view.device_id[17] = '\0';
    client->view.connected = true; client->view.bound = false; client->view.generation = generation;
    if (client->view.request.valid && !client->prepare_sent) { client->view.request.valid = false; }
    client->view.desktop_client_id[0] = '\0';
    pending_clear(client); client->hello_needed = true; client->intent.operation = OP_NONE; client->refresh_turn = 0;
    client->recovery_needed = client->view.request.valid && !terminal(client->view.request.status);
    client->view.request.capture_pending = false;
    if (client->recovery_needed) { client->view.request.uncertain = true; }
    clear_error(client); changed(client); linx_gateway_client_tick(client, now);
}

void linx_gateway_client_disconnected(linx_gateway_client_t *client, uint32_t generation)
{
    if (!client || generation != client->view.generation || !client->view.connected) { return; }
    client->view.connected = false; client->view.bound = false; client->view.request.capture_pending = false;
    if (client->view.request.valid && !client->prepare_sent) { client->view.request.valid = false; }
    if (client->view.request.valid && !terminal(client->view.request.status)) {
        client->view.request.uncertain = true; client->recovery_needed = true;
    }
    pending_clear(client); client->intent.operation = OP_NONE;
    fail(client, LINX_GATEWAY_ERROR_DISCONNECTED, "连接已断开，保留上次数据");
}

void linx_gateway_client_transport_error(linx_gateway_client_t *client, uint32_t generation)
{
    if (!client || !client->view.configured || generation != client->view.generation) { return; }
    linx_gateway_client_disconnected(client, generation);
    fail(client, LINX_GATEWAY_ERROR_TRANSPORT, "网关连接失败，请检查网络与服务配置");
}

static void schedule(linx_gateway_client_t *client, uint32_t now)
{
    if (!client->view.connected || client->pending.operation) { return; }
    if (client->hello_needed) {
        if (send_operation(client, OP_HELLO, "", "", "", 0, now)) { client->hello_needed = false; }
        return;
    }
    if (!client->view.bound) { return; }
    linx_gateway_request_t *request = &client->view.request;
    if (client->recovery_needed && request->valid) {
        if (send_operation(client, OP_GET, "", "", "", 0, now)) { client->recovery_needed = false; }
        return;
    }
    if (request->cancel_requested && !request->cancel_sent && !request->uncertain &&
        (request->status == LINX_GATEWAY_REQUEST_AWAITING_TEXT || request->status == LINX_GATEWAY_REQUEST_AWAITING_CONFIRMATION)) {
        send_operation(client, OP_CANCEL, "", "", "", 0, now); return;
    }
    if (request->valid && request->confirm_requested && !request->confirm_sent && !request->cancel_requested &&
        !request->uncertain && request->text_present && request->status == LINX_GATEWAY_REQUEST_AWAITING_CONFIRMATION) {
        send_operation(client, OP_CONFIRM, "", "", "", 0, now); return;
    }
    if (request->valid && !client->prepare_sent && !request->cancel_requested) {
        send_operation(client, OP_PREPARE, request->agent_session_id, "", "", 0, now); return;
    }
    if (client->intent.operation) {
        unsigned operation = client->intent.operation;
        if (send_operation(client, operation, client->intent.session, client->intent.message,
                           client->intent.cursor, client->intent.offset, now)) { client->intent.operation = OP_NONE; }
        return;
    }
    for (unsigned i = 0; i < 3; ++i) {
        unsigned turn = (client->refresh_turn + i) % 3;
        if (turn == 0 && client->sessions_dirty) {
            if (send_operation(client, OP_SESSIONS, "", "", "", 0, now)) {
                client->sessions_dirty = false; client->sessions_depth = 0; client->refresh_turn = 1;
            }
            return;
        }
        if (turn == 1 && client->detail_dirty && *client->view.selected_session_id) {
            if (send_operation(client, OP_DETAIL, client->view.selected_session_id, "", "", 0, now)) {
                client->detail_dirty = false; client->refresh_turn = 2;
            }
            return;
        }
        if (turn == 2 && client->history_dirty && *client->view.history_session_id) {
            if (send_operation(client, OP_HISTORY, client->view.history_session_id, "", "", 0, now)) {
                client->history_dirty = false; client->history_depth = 0; client->refresh_turn = 0;
            }
            return;
        }
    }
}

void linx_gateway_client_tick(linx_gateway_client_t *client, uint32_t now)
{
    if (!client) { return; }
    if (client->pending.operation && (uint32_t)(now - client->pending.sent_at) >= LINX_GATEWAY_TIMEOUT_MS) {
        unsigned operation = client->pending.operation;
        pending_clear(client);
        if (operation >= OP_PREPARE) {
            client->view.request.uncertain = true; client->view.request.capture_pending = false;
            /* 一次只读恢复；get 本身超时等待重连或用户重试，避免 busy loop。 */
            if (operation != OP_GET) { client->recovery_needed = true; }
        }
        fail(client, LINX_GATEWAY_ERROR_TIMEOUT, "服务应答超时");
    }
    schedule(client, now);
}

static bool transition(linx_gateway_request_status_t from, linx_gateway_request_status_t to)
{
    if (from == to || from == LINX_GATEWAY_REQUEST_NONE) { return true; }
    if (terminal(from)) { return false; }
    if (from == LINX_GATEWAY_REQUEST_UNKNOWN) {
        return to == LINX_GATEWAY_REQUEST_SENDING || to == LINX_GATEWAY_REQUEST_RUNNING ||
            to == LINX_GATEWAY_REQUEST_COMPLETED || to == LINX_GATEWAY_REQUEST_FAILED;
    }
    if (from == LINX_GATEWAY_REQUEST_AWAITING_TEXT) {
        return to == LINX_GATEWAY_REQUEST_AWAITING_CONFIRMATION || to == LINX_GATEWAY_REQUEST_FAILED || to == LINX_GATEWAY_REQUEST_CANCELLED;
    }
    if (from == LINX_GATEWAY_REQUEST_AWAITING_CONFIRMATION) { return to >= LINX_GATEWAY_REQUEST_SENDING; }
    if (from == LINX_GATEWAY_REQUEST_SENDING) {
        return to == LINX_GATEWAY_REQUEST_RUNNING || to == LINX_GATEWAY_REQUEST_COMPLETED ||
            to == LINX_GATEWAY_REQUEST_FAILED || to == LINX_GATEWAY_REQUEST_UNKNOWN;
    }
    return to == LINX_GATEWAY_REQUEST_COMPLETED || to == LINX_GATEWAY_REQUEST_FAILED || to == LINX_GATEWAY_REQUEST_UNKNOWN;
}

static bool request_apply(linx_gateway_client_t *client, const cJSON *data, unsigned operation)
{
    linx_gateway_request_t *request = &client->view.request;
    if (!request->valid || strcmp(request->request_id, string(data, "request_id")) ||
        strcmp(request->agent_session_id, string(data, "agent_session_id")) ||
        (request->status != LINX_GATEWAY_REQUEST_NONE && strcmp(request->target_title, string(data, "target_title")))) {
        return fail(client, LINX_GATEWAY_ERROR_INVALID, "请求目标不匹配");
    }
    if (!fits(data, "target_title", LINX_GATEWAY_TITLE_CAPACITY) || !fits(data, "text", LINX_GATEWAY_TEXT_MAX_BYTES + 1U)) {
        return fail(client, LINX_GATEWAY_ERROR_CAPACITY, "请求文字超出设备容量");
    }
    const cJSON *error = field(data, "error");
    if (!fits(error, "message", LINX_GATEWAY_SUMMARY_CAPACITY)) { return fail(client, LINX_GATEWAY_ERROR_CAPACITY, "错误文字超出设备容量"); }
    bool text_present = cJSON_IsString(field(data, "text"));
    linx_gateway_request_status_t status = LINX_GATEWAY_REQUEST_NONE;
    for (unsigned i = 1; i < sizeof(request_statuses) / sizeof(request_statuses[0]); ++i) {
        if (!strcmp(request_statuses[i], string(data, "status"))) { status = (linx_gateway_request_status_t)i; }
    }
    /* 终态吸收；原文只校验长度与契约，持久化事实由Gateway负责。 */
    if (!transition(request->status, status)) return true;
    client->prepare_rejected = false; /* 有效快照证明原请求存在，后续 NOT_FOUND 不能释放未知结果。 */
    if (request->status == LINX_GATEWAY_REQUEST_NONE) { copy_string(request->target_title, string(data, "target_title")); }
    request->status = status;
    request->text_present |= text_present;
    /* 只读核对仍待确认时，不能清除明确的确认失败。 */
    if (!(request->confirm_sent && status == LINX_GATEWAY_REQUEST_AWAITING_CONFIRMATION && cJSON_IsNull(error))) {
        copy_string(request->error_code, string(error, "code"));
        copy_string(request->error_message, string(error, "message"));
    }
    bool was_uncertain = request->uncertain;
    request->uncertain = false;
    if (operation == OP_PREPARE && status == LINX_GATEWAY_REQUEST_AWAITING_TEXT && !was_uncertain &&
        !request->cancel_requested && !request->capture_claimed) { request->capture_pending = true; }
    if (status != LINX_GATEWAY_REQUEST_AWAITING_TEXT || request->cancel_requested) {
        request->capture_pending = false;
    }
    if (terminal(status)) {
        request->confirm_requested = request->confirm_sent = false;
        client->recovery_needed = false;
        if (client->pending.operation >= OP_PREPARE && !strcmp(client->pending.request, request->request_id)) { pending_clear(client); }
    }
    return true;
}

static bool binding_apply(linx_gateway_client_t *client, const cJSON *data)
{
    if (strcmp(string(data, "device_id"), client->view.device_id)) { return fail(client, LINX_GATEWAY_ERROR_INVALID, "绑定设备不匹配"); }
    bool was_bound = client->view.bound;
    client->view.bound = cJSON_IsString(field(data, "desktop_client_id"));
    copy_string(client->view.desktop_client_id, string(data, "desktop_client_id"));
    if (client->view.bound && !was_bound) {
        client->sessions_dirty = true;
        client->detail_dirty = *client->view.selected_session_id != '\0';
        client->history_dirty = *client->view.history_session_id != '\0';
    }
    return true;
}

static bool success_apply(linx_gateway_client_t *client, unsigned operation, const cJSON *data)
{
    linx_gateway_view_t *view = &client->view;
    if (operation == OP_HELLO) { return binding_apply(client, data); }
    if (operation >= OP_PREPARE) { return request_apply(client, data, operation); }
    if (operation == OP_SESSIONS) {
        const cJSON *items = field(data, "items");
        if (duplicates(items, "agent_session_id")) { return fail(client, LINX_GATEWAY_ERROR_INVALID, "会话 ID 重复"); }
        const cJSON *cached = items->child;
        for (size_t i = 0; cached && i < LINX_GATEWAY_SESSION_MAX; ++i, cached = cached->next) {
            if (!session_fits(cached)) { return fail(client, LINX_GATEWAY_ERROR_CAPACITY, "会话文字超出设备容量"); }
        }
        size_t count = 0;
        for (const cJSON *item = items->child; item && count < LINX_GATEWAY_SESSION_MAX; item = item->next) { session_copy(&view->sessions[count++], item); }
        view->sessions_omitted = cJSON_GetArraySize(items) > (int)count;
        view->session_count = count;
        copy_string(view->sessions_next_cursor, string(data, "next_cursor"));
        if (!*view->selected_session_id && count) {
            copy_string(view->selected_session_id, view->sessions[0].agent_session_id);
            client->detail_dirty = true;
        }
        return true;
    }
    if (operation == OP_DETAIL) {
        if (strcmp(client->pending.session, string(data, "agent_session_id"))) { return fail(client, LINX_GATEWAY_ERROR_INVALID, "会话目标不匹配"); }
        if (!session_fits(data)) { return fail(client, LINX_GATEWAY_ERROR_CAPACITY, "会话文字超出设备容量"); }
        if (!strcmp(view->selected_session_id, client->pending.session)) {
            session_copy(&view->detail, data); view->detail_valid = true; view->target_unavailable = false;
        }
        return true;
    }
    if (operation == OP_HISTORY) {
        const cJSON *items = field(data, "items");
        if (duplicates(items, "message_id")) { return fail(client, LINX_GATEWAY_ERROR_INVALID, "消息 ID 重复"); }
        if (strcmp(view->history_session_id, client->pending.session)) { return true; }
        size_t count = 0;
        for (const cJSON *item = items->child; item && count < LINX_GATEWAY_HISTORY_MAX; item = item->next) {
            copy_string(view->messages[count].message_id, string(item, "message_id"));
            copy_string(view->messages[count++].role, string(item, "role"));
        }
        view->messages_omitted = cJSON_GetArraySize(items) > (int)count;
        view->message_count = count;
        copy_string(view->history_next_cursor, string(data, "next_cursor"));
        return true;
    }
    if (!fits(data, "text", LINX_GATEWAY_CONTENT_CAPACITY)) { return fail(client, LINX_GATEWAY_ERROR_CAPACITY, "正文分块超出设备容量"); }
    const cJSON *next = field(data, "next_offset");
    bool has_next = cJSON_IsNumber(next);
    size_t text_bytes = strlen(string(data, "text"));
    if (has_next && (next->valuedouble > UINT32_MAX || next->valuedouble <= client->pending.offset ||
        text_bytes > UINT32_MAX - client->pending.offset || next->valuedouble != client->pending.offset + text_bytes)) {
        return fail(client, LINX_GATEWAY_ERROR_INVALID, "正文偏移与 UTF-8 字节数不匹配");
    }
    if (strcmp(view->selected_session_id, client->pending.session) ||
        (client->intent.operation == OP_CONTENT && strcmp(client->intent.message, client->pending.message))) { return true; }
    bool same = view->content_valid && !strcmp(view->content_session_id, client->pending.session) &&
        !strcmp(view->content_message_id, client->pending.message);
    size_t index = 0;
    if (same) {
        while (index < client->block_count && client->block_offsets[index] != client->pending.offset) { ++index; }
        if (index == client->block_count && client->block_count == LINX_GATEWAY_BLOCK_MAX) {
            return fail(client, LINX_GATEWAY_ERROR_NAVIGATION, "正文导航已满，请从首块重开");
        }
    } else { client->block_count = 0; }
    if (index == client->block_count) { client->block_offsets[client->block_count++] = client->pending.offset; }
    client->block_index = index;
    copy_string(view->content_session_id, client->pending.session);
    copy_string(view->content_message_id, client->pending.message);
    copy_string(view->content, string(data, "text"));
    view->content_offset = client->pending.offset; view->content_has_next = has_next;
    view->content_next_offset = has_next ? (uint32_t)next->valuedouble : 0;
    view->content_valid = true;
    return true;
}

static bool remote_error(linx_gateway_client_t *client, unsigned operation, const cJSON *error)
{
    if (!fits(error, "message", LINX_GATEWAY_SUMMARY_CAPACITY)) { return fail(client, LINX_GATEWAY_ERROR_CAPACITY, "错误文字超出设备容量"); }
    client->view.error = LINX_GATEWAY_ERROR_REMOTE;
    copy_string(client->view.error_code, string(error, "code"));
    copy_string(client->view.error_message, string(error, "message"));
    if (operation == OP_DETAIL && !strcmp(string(error, "code"), "NOT_FOUND") &&
        !strcmp(client->pending.session, client->view.selected_session_id)) {
        client->view.target_unavailable = true;
    }
    if ((operation == OP_SESSIONS || operation == OP_HISTORY) && !strcmp(string(error, "code"), "CONFLICT") && *client->pending.cursor) {
        if (operation == OP_SESSIONS) { client->sessions_depth = 0; client->sessions_dirty = true; }
        else { client->history_depth = 0; client->history_dirty = true; }
    }
    linx_gateway_request_t *request = &client->view.request;
    const char *code = string(error, "code");
    if (operation == OP_PREPARE && request->status == LINX_GATEWAY_REQUEST_NONE &&
        (!strcmp(code, "NOT_FOUND") || !strcmp(code, "INVALID_ARGUMENT") ||
         !strcmp(code, "UNAUTHENTICATED") || !strcmp(code, "PERMISSION_DENIED") || !strcmp(code, "CONFLICT"))) {
        client->prepare_rejected = true;
        copy_string(request->error_code, code); copy_string(request->error_message, string(error, "message"));
    }
    if (operation == OP_GET && !strcmp(code, "NOT_FOUND") && client->prepare_rejected &&
        request->status == LINX_GATEWAY_REQUEST_NONE && !request->confirm_requested && !request->confirm_sent &&
        !request->cancel_sent && !request->capture_claimed) {
        /* 准备明确拒绝 + 核对不存在：仅释放未录音、未提交的本地名额，保留原始错误。 */
        request->valid = false; request->uncertain = false; request->capture_pending = false;
        request->cancel_requested = false; request->request_id[0] = '\0';
        client->prepare_sent = false;
        client->recovery_needed = false;
        client->prepare_rejected = false;
    }
    if (operation == OP_CONFIRM) {
        copy_string(request->error_code, code); copy_string(request->error_message, string(error, "message"));
    }
    if (operation == OP_CANCEL || operation == OP_PREPARE || operation == OP_CONFIRM) {
        request->uncertain = true; request->capture_pending = false;
        client->recovery_needed = true;
    }
    return true;
}

bool linx_gateway_client_receive(linx_gateway_client_t *client, uint32_t generation,
                                 const char *bytes, size_t size, uint32_t now)
{
    if (!client) return false;
    if (!client->view.connected || generation != client->view.generation) {
        protocol_log(client, generation, "rx_ignored", "", OP_NONE, size, NULL, NULL); return false;
    }
    if (!preflight(bytes, size)) {
        protocol_log(client, generation, "rx_invalid", "", OP_NONE, size, NULL, NULL);
        return fail(client, LINX_GATEWAY_ERROR_INVALID, "服务消息格式无效或超出解析上限");
    }
    cJSON *root = parse(bytes, size);
    if (!root) {
        protocol_log(client, generation, "rx_invalid", "", OP_NONE, size, NULL, NULL);
        return fail(client, LINX_GATEWAY_ERROR_MEMORY, "服务消息内存不足");
    }
    const char *type = string(root, "type");
    bool valid = false, ignored = false;
    unsigned logged_operation = OP_NONE;
    const cJSON *logged_data = NULL, *logged_error = NULL;
    if (!strcmp(type, "response")) {
        unsigned operation = client->pending.operation;
        if (!operation || generation != client->pending.generation || strcmp(string(root, "reply_to"), client->pending.id)) {
            protocol_log(client, generation, "rx_ignored", "response", OP_NONE, size, NULL, NULL);
            cJSON_Delete(root); return false;
        }
        logged_operation = operation;
        char definition[64]; snprintf(definition, sizeof(definition), "%s.response", operations[operation]);
        if (definition_valid(definition, root)) {
            const cJSON *payload = field(root, "payload");
            if (cJSON_IsTrue(field(payload, "ok"))) {
                if (operation >= OP_PREPARE) {
                    logged_data = field(payload, "data"); logged_error = field(logged_data, "error");
                }
                valid = success_apply(client, operation, field(payload, "data"));
                if (valid) { clear_error(client); }
            } else {
                logged_error = field(payload, "error");
                valid = remote_error(client, operation, logged_error);
            }
            if (valid) { pending_clear(client); }
        } else { fail(client, LINX_GATEWAY_ERROR_INVALID, "服务应答不符合原操作契约"); }
    } else if (definition_valid(type, root)) {
        const cJSON *payload = field(root, "payload");
        if (!strcmp(type, "device.bound")) { valid = binding_apply(client, payload); }
        else if (client->view.bound && (!strcmp(type, "request.proposed") || !strcmp(type, "request.updated"))) {
            logged_data = payload; logged_error = field(payload, "error");
            valid = request_apply(client, payload, OP_NONE);
        } else if (client->view.bound && !strcmp(type, "agent_sessions.updated")) {
            client->sessions_dirty = true; client->detail_dirty = *client->view.selected_session_id != '\0'; valid = true;
        } else if (client->view.bound && !strcmp(type, "message.available")) {
            if (!strcmp(string(payload, "agent_session_id"), client->view.selected_session_id)) {
                client->view.new_reply = true; client->detail_dirty = true;
                copy_string(client->view.reply_message_id, string(payload, "message_id"));
                ++client->view.reply_revision;
                client->history_dirty = *client->view.history_session_id != '\0';
            }
            valid = true;
        } else ignored = true;
    } else { fail(client, LINX_GATEWAY_ERROR_INVALID, "服务通知不符合契约"); }
    protocol_log(client, generation, valid ? "rx" : ignored ? "rx_ignored" : "rx_invalid", type,
                 logged_operation, size, logged_data, logged_error);
    cJSON_Delete(root);
    if (valid) { changed(client); schedule(client, now); }
    return valid;
}

static void set_intent(linx_gateway_client_t *client, unsigned operation, const char *session,
                        const char *message, const char *cursor, uint32_t offset)
{
    client->intent.operation = operation;
    copy_string(client->intent.session, session); copy_string(client->intent.message, message);
    copy_string(client->intent.cursor, cursor); client->intent.offset = offset;
}

bool linx_gateway_client_action(linx_gateway_client_t *client, linx_gateway_action_t action,
                                const char *target, uint32_t now)
{
    if (!client) { return false; }
    linx_gateway_view_t *view = &client->view;
    linx_gateway_request_t *request = &view->request;
    if (action == LINX_GATEWAY_ACTION_CONFIRM) {
        if (!view->configured) { return fail(client, LINX_GATEWAY_ERROR_UNCONFIGURED, "服务未配置"); }
        if (!request->valid || request->cancel_requested ||
            (request->status != LINX_GATEWAY_REQUEST_AWAITING_TEXT && request->status != LINX_GATEWAY_REQUEST_AWAITING_CONFIRMATION)) {
            return fail(client, LINX_GATEWAY_ERROR_INVALID, "当前请求不能确认");
        }
        request->confirm_requested = true;
        clear_error(client); changed(client); schedule(client, now); return true;
    }
    if (action == LINX_GATEWAY_ACTION_CANCEL && request->valid &&
        (request->status == LINX_GATEWAY_REQUEST_NONE || request->status == LINX_GATEWAY_REQUEST_AWAITING_TEXT ||
         request->status == LINX_GATEWAY_REQUEST_AWAITING_CONFIRMATION)) {
        request->capture_pending = false; request->confirm_requested = false; request->cancel_requested = true;
        if (!client->prepare_sent) { request->valid = false; }
        changed(client); schedule(client, now); return true;
    }
    if (!view->configured) { return fail(client, LINX_GATEWAY_ERROR_UNCONFIGURED, "服务未配置"); }
    if (!view->connected) { return fail(client, LINX_GATEWAY_ERROR_DISCONNECTED, "服务未连接"); }
    if (!view->bound) {
        if (action == LINX_GATEWAY_ACTION_REFRESH) {
            if (!client->pending.operation) { client->hello_needed = true; }
            clear_error(client); schedule(client, now); return true;
        }
        return fail(client, LINX_GATEWAY_ERROR_INVALID, "请先在电脑端绑定设备");
    }
    const char *session = target && (action == LINX_GATEWAY_ACTION_SELECT || action == LINX_GATEWAY_ACTION_HISTORY ||
        action == LINX_GATEWAY_ACTION_PREPARE) ? target : view->selected_session_id;
    if (action == LINX_GATEWAY_ACTION_REFRESH) {
        client->sessions_dirty = false; client->detail_dirty = *view->selected_session_id != '\0';
        client->sessions_depth = 0; set_intent(client, OP_SESSIONS, "", "", "", 0);
    } else if (action == LINX_GATEWAY_ACTION_SESSIONS_NEXT || action == LINX_GATEWAY_ACTION_HISTORY_NEXT) {
        bool sessions = action == LINX_GATEWAY_ACTION_SESSIONS_NEXT;
        size_t *depth = sessions ? &client->sessions_depth : &client->history_depth;
        char (*stack)[LINX_GATEWAY_ID_CAPACITY] = sessions ? client->sessions_cursor : client->history_cursor;
        const char *next = sessions ? view->sessions_next_cursor : view->history_next_cursor;
        if (!*next || *depth == LINX_GATEWAY_NAV_MAX) { return fail(client, LINX_GATEWAY_ERROR_NAVIGATION, "已到分页边界，请返回首批"); }
        copy_string(stack[(*depth)++], next);
        set_intent(client, sessions ? OP_SESSIONS : OP_HISTORY, sessions ? "" : view->history_session_id, "", next, 0);
    } else if (action == LINX_GATEWAY_ACTION_SESSIONS_PREVIOUS || action == LINX_GATEWAY_ACTION_HISTORY_PREVIOUS) {
        bool sessions = action == LINX_GATEWAY_ACTION_SESSIONS_PREVIOUS;
        size_t *depth = sessions ? &client->sessions_depth : &client->history_depth;
        char (*stack)[LINX_GATEWAY_ID_CAPACITY] = sessions ? client->sessions_cursor : client->history_cursor;
        if (!*depth) { return fail(client, LINX_GATEWAY_ERROR_NAVIGATION, "已是首批"); }
        --*depth;
        set_intent(client, sessions ? OP_SESSIONS : OP_HISTORY, sessions ? "" : view->history_session_id,
                   "", *depth ? stack[*depth - 1] : "", 0);
    } else if (action == LINX_GATEWAY_ACTION_SELECT || action == LINX_GATEWAY_ACTION_HISTORY) {
        if (!id_valid(session)) { return fail(client, LINX_GATEWAY_ERROR_INVALID, "会话 ID 无效"); }
        if (action == LINX_GATEWAY_ACTION_SELECT) {
            if (strcmp(session, view->selected_session_id)) {
                view->detail_valid = false; view->target_unavailable = false; view->new_reply = false;
                view->reply_message_id[0] = '\0';
            }
            copy_string(view->selected_session_id, session); client->detail_dirty = false;
            set_intent(client, OP_DETAIL, session, "", "", 0);
        } else {
            if (strcmp(view->history_session_id, session)) {
                /* 跨目标时原子清空缓存与游标，不能把旧 message_id 标成新会话的消息。 */
                view->message_count = 0; view->messages_omitted = false; view->history_next_cursor[0] = '\0';
            }
            copy_string(view->history_session_id, session); client->history_depth = 0; client->history_dirty = false;
            set_intent(client, OP_HISTORY, session, "", "", 0);
        }
    } else if (action == LINX_GATEWAY_ACTION_CONTENT || action == LINX_GATEWAY_ACTION_CONTENT_NEXT || action == LINX_GATEWAY_ACTION_CONTENT_PREVIOUS) {
        if (action == LINX_GATEWAY_ACTION_CONTENT) {
            if (!id_valid(target) || !id_valid(session)) { return fail(client, LINX_GATEWAY_ERROR_INVALID, "消息 ID 无效"); }
            set_intent(client, OP_CONTENT, session, target, "", 0);
        } else {
            if (!view->content_valid || (action == LINX_GATEWAY_ACTION_CONTENT_NEXT && !view->content_has_next) ||
                (action == LINX_GATEWAY_ACTION_CONTENT_PREVIOUS && client->block_index == 0)) {
                return fail(client, LINX_GATEWAY_ERROR_NAVIGATION, "已到正文边界");
            }
            uint32_t offset = action == LINX_GATEWAY_ACTION_CONTENT_NEXT ? view->content_next_offset : client->block_offsets[client->block_index - 1];
            if (action == LINX_GATEWAY_ACTION_CONTENT_NEXT && client->block_index + 1 == client->block_count && client->block_count == LINX_GATEWAY_BLOCK_MAX) {
                return fail(client, LINX_GATEWAY_ERROR_NAVIGATION, "正文导航已满，请从首块重开");
            }
            set_intent(client, OP_CONTENT, view->content_session_id, view->content_message_id, "", offset);
        }
    } else if (action == LINX_GATEWAY_ACTION_PREPARE) {
        if ((request->valid && !terminal(request->status)) || client->pending.operation >= OP_PREPARE || client->recovery_needed) {
            return fail(client, LINX_GATEWAY_ERROR_BUSY, "请先核对当前请求");
        }
        if (!id_valid(session) || !view->detail_valid || view->target_unavailable || strcmp(view->detail.agent_session_id, session)) {
            return fail(client, LINX_GATEWAY_ERROR_INVALID, "当前会话不可用");
        }
        memset(request, 0, sizeof(*request)); request->valid = true;
        if (!make_id(client, "request", request->request_id, sizeof(request->request_id))) { request->valid = false; return false; }
        copy_string(request->agent_session_id, session); copy_string(request->target_title, view->detail.title);
        client->prepare_sent = false; client->prepare_rejected = false;
    } else if (action == LINX_GATEWAY_ACTION_REQUEST_GET) {
        if (!request->valid) { return fail(client, LINX_GATEWAY_ERROR_INVALID, "没有待核对请求"); }
        client->recovery_needed = true;
    } else { return fail(client, LINX_GATEWAY_ERROR_INVALID, "当前操作不可用"); }
    clear_error(client); changed(client); schedule(client, now); return true;
}

const linx_gateway_view_t *linx_gateway_client_view(const linx_gateway_client_t *client)
{
    return client ? &client->view : NULL;
}

bool linx_gateway_client_claim_capture(linx_gateway_client_t *client)
{
    if (!client || !client->view.request.capture_pending) { return false; }
    client->view.request.capture_pending = false; client->view.request.capture_claimed = true;
    changed(client); return true;
}

void linx_gateway_client_drop_transient(linx_gateway_client_t *client, bool history)
{
    if (!client) return;
    linx_gateway_view_t *view = &client->view;
    /* Forget the packet identity as well: a late display reply cannot refill the cache. */
    if (client->pending.operation == OP_CONTENT || (history && client->pending.operation == OP_HISTORY))
        pending_clear(client);
    if (client->intent.operation == OP_CONTENT || (history && client->intent.operation == OP_HISTORY))
        client->intent.operation = OP_NONE;
    view->content_valid = view->content_has_next = false;
    view->content_offset = view->content_next_offset = 0;
    memset(view->content, 0, sizeof(view->content));
    view->content_session_id[0] = view->content_message_id[0] = '\0';
    memset(client->block_offsets, 0, sizeof(client->block_offsets));
    client->block_count = client->block_index = 0;
    if (history) {
        memset(view->messages, 0, sizeof(view->messages));
        view->message_count = 0; view->messages_omitted = false;
        view->history_next_cursor[0] = view->history_session_id[0] = '\0';
        memset(client->history_cursor, 0, sizeof(client->history_cursor));
        client->history_depth = 0; client->history_dirty = false;
    }
    changed(client);
}

bool linx_gateway_client_content_pending(const linx_gateway_client_t *client)
{
    return client && (client->pending.operation == OP_CONTENT || client->intent.operation == OP_CONTENT);
}

bool linx_gateway_client_mark_reply_read(linx_gateway_client_t *client, uint32_t reply_revision)
{
    if (!client) return false;
    linx_gateway_view_t *view = &client->view;
    if (!view->new_reply || view->reply_revision != reply_revision ||
        !view->content_valid || !view->content[0] || linx_gateway_client_content_pending(client) ||
        strcmp(view->content_session_id, view->selected_session_id) ||
        strcmp(view->content_message_id, view->reply_message_id)) return false;
    view->new_reply = false;
    changed(client);
    return true;
}
