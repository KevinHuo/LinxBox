#include "linx_gateway_client.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"

static void *budget_malloc(size_t size);
static void budget_free(void *pointer);
#define malloc budget_malloc
#define free budget_free
#include "../main/linx_gateway_client.c"
#undef malloc
#undef free

/* 包括解析器自身分配与cJSON节点，模拟接收帧已借出后的剩余堆预算。 */
static struct { void *pointer; size_t size; } budget_blocks[512];
static bool budget_active;
static size_t budget_live, budget_peak, budget_limit, budget_calls, budget_denied;
static long budget_fail_at = -1;
static void *budget_malloc(size_t size)
{
    if (!budget_active) return malloc(size);
    size_t call = budget_calls++;
    if ((long)call == budget_fail_at || size > budget_limit - budget_live) {
        budget_denied = budget_live + size;
        return NULL;
    }
    void *pointer = malloc(size);
    if (!pointer) return NULL;
    size_t index = 0; while (index < 512U && budget_blocks[index].pointer) ++index;
    assert(index < 512U); budget_blocks[index].pointer = pointer; budget_blocks[index].size = size;
    budget_live += size; if (budget_live > budget_peak) budget_peak = budget_live;
    return pointer;
}
static void budget_free(void *pointer)
{
    if (budget_active && pointer) {
        size_t index = 0; while (index < 512U && budget_blocks[index].pointer != pointer) ++index;
        assert(index < 512U); budget_live -= budget_blocks[index].size;
        budget_blocks[index].pointer = NULL; budget_blocks[index].size = 0;
    }
    free(pointer);
}
static void budget_begin(long fail_at)
{
    assert(!budget_live);
    budget_active = true; budget_peak = budget_calls = budget_denied = 0; budget_fail_at = fail_at;
    budget_limit = 32U * 1024U;
    cJSON_Hooks hooks = {.malloc_fn = budget_malloc, .free_fn = budget_free}; cJSON_InitHooks(&hooks);
}
static void budget_end(void)
{
    assert(!budget_live); budget_active = false; cJSON_InitHooks(NULL);
}

static linx_gateway_client_t client;
static char sent[16384];
static unsigned send_count, confirmations, cancellations, preparations;
static bool accept_send = true;
static bool injecting_failure;
static uint32_t clock_now = 10;

static bool send_json(uint32_t generation, const char *json, size_t size, void *user)
{
    assert(generation == client.view.generation && user == &client);
    assert(size < sizeof(sent));
    if (!accept_send) { return false; }
    memcpy(sent, json, size); sent[size] = '\0'; ++send_count;
    if (injecting_failure) { return true; }
    cJSON *root = cJSON_Parse(sent);
    const char *type = cJSON_GetObjectItemCaseSensitive(root, "type")->valuestring;
    assert(linx_gateway_client_validate_json(type, sent, size));
    assert(strcmp(type, "request.confirm") && strcmp(type, "request.propose"));
    assert(!strstr(json, "\"text\":") && !strstr(json, "\"limit\":"));
    cancellations += !strcmp(type, "request.cancel");
    preparations += !strcmp(type, "request.prepare");
    cJSON_Delete(root);
    return true;
}

static bool deliver(cJSON *root)
{
    char *json = cJSON_PrintUnformatted(root); assert(json);
    bool ok = linx_gateway_client_receive(&client, client.view.generation, json, strlen(json), clock_now++);
    cJSON_free(json); cJSON_Delete(root); return ok;
}

static cJSON *parsed(const char *json)
{
    cJSON *root = cJSON_Parse(json); assert(root); return root;
}

static bool response(cJSON *data)
{
    cJSON *root = cJSON_CreateObject(), *payload = cJSON_AddObjectToObject(root, "payload");
    cJSON_AddStringToObject(root, "id", "reply"); cJSON_AddStringToObject(root, "type", "response");
    cJSON_AddStringToObject(root, "reply_to", client.pending.id);
    cJSON_AddTrueToObject(payload, "ok"); cJSON_AddItemToObject(payload, "data", data);
    return deliver(root);
}

static bool error_response(const char *code, const char *message)
{
    cJSON *root = cJSON_CreateObject(), *payload = cJSON_AddObjectToObject(root, "payload");
    cJSON_AddStringToObject(root, "id", "reply"); cJSON_AddStringToObject(root, "type", "response");
    cJSON_AddStringToObject(root, "reply_to", client.pending.id);
    cJSON_AddFalseToObject(payload, "ok");
    cJSON *error = cJSON_AddObjectToObject(payload, "error");
    cJSON_AddStringToObject(error, "code", code); cJSON_AddStringToObject(error, "message", message);
    return deliver(root);
}

static bool notification(const char *type, cJSON *payload)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "id", "notice"); cJSON_AddStringToObject(root, "type", type);
    cJSON_AddItemToObject(root, "payload", payload); return deliver(root);
}

static cJSON *session(const char *id)
{
    cJSON *value = parsed("{\"agent_session_id\":\"session\",\"title\":\"真实标题\",\"project_name\":\"项目\",\"agent\":\"Codex\",\"status\":\"idle\",\"summary\":\"摘要\"}");
    cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(value, "agent_session_id"), id);
    return value;
}

static cJSON *page(unsigned count, bool messages, const char *next)
{
    cJSON *value = cJSON_CreateObject(), *items = cJSON_AddArrayToObject(value, "items");
    for (unsigned i = 0; i < count; ++i) {
        char id[32]; snprintf(id, sizeof(id), "item-%u", i);
        cJSON *item = messages ? parsed("{\"message_id\":\"x\",\"role\":\"assistant\",\"summary\":\"真实历史\"}") : session(id);
        if (messages) { cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(item, "message_id"), id); }
        cJSON_AddItemToArray(items, item);
    }
    if (next) { cJSON_AddStringToObject(value, "next_cursor", next); }
    else { cJSON_AddNullToObject(value, "next_cursor"); }
    return value;
}

static cJSON *chunk(const char *text, int next)
{
    cJSON *value = cJSON_CreateObject(); cJSON_AddStringToObject(value, "text", text);
    if (next >= 0) { cJSON_AddNumberToObject(value, "next_offset", next); }
    else { cJSON_AddNullToObject(value, "next_offset"); }
    return value;
}

static cJSON *request(const char *status, const char *text)
{
    cJSON *value = cJSON_CreateObject();
    cJSON_AddStringToObject(value, "request_id", client.view.request.request_id);
    cJSON_AddStringToObject(value, "agent_session_id", client.view.request.agent_session_id);
    cJSON_AddStringToObject(value, "target_title", client.view.request.target_title);
    cJSON_AddStringToObject(value, "status", status); cJSON_AddNullToObject(value, "error");
    if (text) { cJSON_AddStringToObject(value, "text", text); } else { cJSON_AddNullToObject(value, "text"); }
    return value;
}

static void start(bool bound)
{
    accept_send = true; confirmations = cancellations = preparations = send_count = 0;
    linx_gateway_client_config_t config = {.configured = true, .instance_id = 0x123456789abcdef0ULL, .send = send_json, .user = &client};
    linx_gateway_client_deinit(&client); linx_gateway_client_init(&client, &config);
    linx_gateway_client_connected(&client, 3, "aa-bb-cc-dd-ee-ff", clock_now++);
    assert(!strcmp(client.view.device_id, "AA:BB:CC:DD:EE:FF"));
    assert(response(parsed(bound ? "{\"device_id\":\"AA:BB:CC:DD:EE:FF\",\"desktop_client_id\":\"desktop\"}" :
        "{\"device_id\":\"AA:BB:CC:DD:EE:FF\",\"desktop_client_id\":null}")));
    if (bound) { assert(response(page(0, false, NULL))); }
}

static void select_session(const char *id)
{
    assert(linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_SELECT, id, clock_now++));
    assert(response(session(id))); assert(client.view.detail_valid);
}

static void prepare(void)
{
    select_session("session");
    assert(linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_PREPARE, NULL, clock_now++));
    assert(preparations == 1 && !client.view.request.capture_pending);
}

static void test_binding_and_errors(void)
{
    linx_gateway_client_init(NULL, NULL); linx_gateway_client_tick(NULL, 0);
    linx_gateway_client_connected(NULL, 1, "bad", 0); linx_gateway_client_disconnected(NULL, 0);
    assert(!linx_gateway_client_view(NULL)); assert(!linx_gateway_client_claim_capture(NULL));
    assert(!linx_gateway_client_action(NULL, LINX_GATEWAY_ACTION_REFRESH, NULL, 0));
    assert(!linx_gateway_client_receive(NULL, 1, "{}", 2, 0));
    linx_gateway_client_deinit(&client); linx_gateway_client_init(&client, NULL);
    linx_gateway_client_connected(&client, 1, "bad", 0);
    assert(!linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_REFRESH, NULL, 0));
    start(false);
    assert(!linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_PREPARE, NULL, clock_now));
    assert(!notification("device.bound", parsed("{\"device_id\":\"AA:BB:CC:DD:EE:01\",\"desktop_client_id\":\"d\"}")));
    assert(!notification("device.bound", parsed("{\"device_id\":\"AA:BB:CC:DD:EE:FF\",\"desktop_client_id\":\"\"}")));
    assert(notification("device.bound", parsed("{\"device_id\":\"AA:BB:CC:DD:EE:FF\",\"desktop_client_id\":\"d\"}")));
    assert(response(page(0, false, NULL)));
    assert(linx_gateway_client_view(&client) == &client.view);
    linx_gateway_client_disconnected(&client, 999); assert(client.view.connected);
    linx_gateway_client_disconnected(&client, 3); assert(!client.view.connected);
    linx_gateway_client_disconnected(&client, 3);
    assert(!linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_REFRESH, NULL, clock_now));
    assert(!linx_gateway_client_receive(&client, 3, "{}", 2, 0));
    linx_gateway_client_connected(&client, 4, NULL, 0);
    linx_gateway_client_connected(&client, 4, "bad", 0);
    linx_gateway_client_connected(&client, 4, "aabbccddeeff", clock_now++);
    assert(!linx_gateway_client_receive(&client, 3, "{}", 2, 0));
    assert(error_response("UNAUTHENTICATED", "凭据无效"));
    assert(client.view.error == LINX_GATEWAY_ERROR_REMOTE);
    assert(linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_REFRESH, NULL, clock_now++));
    assert(strstr(sent, "device.hello"));
}

static void test_pages_and_notifications(void)
{
    start(true);
    assert(linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_REFRESH, NULL, clock_now++));
    assert(response(page(3, false, "next\"\\中文")));
    assert(client.view.session_count == 2 && client.view.sessions_omitted); assert(response(session("item-0")));
    assert(linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_SESSIONS_NEXT, NULL, clock_now++));
    assert(!strcmp(client.pending.cursor, "next\"\\中文"));
    assert(response(page(1, false, "next2")));
    assert(linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_SESSIONS_NEXT, NULL, clock_now++));
    assert(error_response("CONFLICT", "游标失效"));
    assert(!*client.pending.cursor); assert(response(page(0, false, NULL)));
    assert(!strcmp(client.view.selected_session_id, "item-0") && !client.view.target_unavailable);
    assert(!linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_SESSIONS_PREVIOUS, NULL, clock_now++));
    assert(!linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_SESSIONS_NEXT, NULL, clock_now++));
    assert(linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_HISTORY, NULL, clock_now++));
    assert(response(page(3, true, "history2"))); assert(client.view.message_count == 2 && client.view.messages_omitted);
    assert(linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_HISTORY_NEXT, NULL, clock_now++));
    assert(response(page(1, true, "history3")));
    assert(linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_HISTORY_PREVIOUS, NULL, clock_now++));
    assert(!*client.pending.cursor); assert(response(page(0, true, NULL)));
    assert(!linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_HISTORY_PREVIOUS, NULL, clock_now++));
    select_session("not-in-first-five");
    assert(linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_REFRESH, NULL, clock_now++));
    for (unsigned i = 0; i < 30; ++i) { assert(notification("agent_sessions.updated", parsed("{}"))); }
    unsigned before = send_count;
    assert(response(page(1, false, NULL))); assert(response(session("not-in-first-five")));
    assert(send_count == before + 2); assert(response(page(0, false, NULL)));
    assert(client.view.detail_valid && !client.view.target_unavailable);
    assert(notification("message.available", parsed("{\"agent_session_id\":\"other\",\"message_id\":\"new\"}")));
    assert(!client.view.new_reply);
    assert(notification("message.available", parsed("{\"agent_session_id\":\"not-in-first-five\",\"message_id\":\"new\"}")));
    assert(client.view.new_reply); assert(response(session("not-in-first-five")));
    assert(response(page(1, true, NULL)));
    assert(linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_SELECT, "gone", clock_now++));
    assert(error_response("NOT_FOUND", "会话已移除")); assert(client.view.target_unavailable);
    assert(!linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_PREPARE, NULL, clock_now++));
    assert(!linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_SELECT, "", clock_now++));
    assert(!linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_CONTENT, NULL, clock_now++));
    assert(!linx_gateway_client_action(&client, (linx_gateway_action_t)999, NULL, clock_now++));
}

static void test_content(void)
{
    start(true); select_session("session");
    assert(!linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_CONTENT_PREVIOUS, NULL, clock_now++));
    assert(linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_CONTENT, "message", clock_now++));
    char text[1026]; memset(text, 'a', 1024); text[1024] = '\0';
    assert(response(chunk(text, 1024))); assert(strlen(client.view.content) == 1024);
    assert(linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_CONTENT_NEXT, NULL, clock_now++));
    assert(client.pending.offset == 1024 && client.view.content_offset == 0);
    assert(notification("message.available", parsed("{\"agent_session_id\":\"session\",\"message_id\":\"new\"}")));
    assert(!strcmp(client.view.content_message_id, "message"));
    assert(response(chunk("第二块", 1033))); assert(client.view.content_offset == 1024);
    /* 显式跨块优先于后台目录详情。 */
    assert(linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_CONTENT_NEXT, NULL, clock_now++));
    assert(response(session("session"))); assert(client.pending.offset == 1033);
    assert(response(chunk("", -1))); assert(client.view.content_valid && !client.view.content_has_next);
    assert(!linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_CONTENT_NEXT, NULL, clock_now++));
    assert(linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_CONTENT_PREVIOUS, NULL, clock_now++));
    assert(client.pending.offset == 1024); assert(response(chunk("第二块", 1033)));
    assert(linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_CONTENT_PREVIOUS, NULL, clock_now++));
    assert(response(chunk("第一块", 9))); assert(client.view.content_offset == 0);
    assert(linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_CONTENT, "oversize", clock_now++));
    text[1024] = 'a'; text[1025] = '\0'; assert(!response(chunk(text, -1)));
    assert(client.view.error == LINX_GATEWAY_ERROR_CAPACITY && !strcmp(client.view.content, "第一块"));
    assert(!response(chunk("bad", 0))); assert(client.view.error == LINX_GATEWAY_ERROR_INVALID);
    assert(error_response("INTERNAL", "读取失败")); assert(!strcmp(client.view.content, "第一块"));
    assert(linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_CONTENT, "new", clock_now++));
    assert(response(chunk("start", 5)));
    for (unsigned i = 1; i < LINX_GATEWAY_BLOCK_MAX; ++i) {
        assert(linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_CONTENT_NEXT, NULL, clock_now++));
        assert(response(chunk("part", 5 + (int)i * 4)));
    }
    assert(!linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_CONTENT_NEXT, NULL, clock_now++));
    assert(client.view.error == LINX_GATEWAY_ERROR_NAVIGATION);
}

static void test_mcp_snapshot_merge(void)
{
    start(true); prepare();
    assert(!linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_PREPARE, NULL, clock_now++));
    assert(response(request("awaiting_text", NULL))); assert(linx_gateway_client_claim_capture(&client));
    assert(!linx_gateway_client_claim_capture(&client));
    assert(notification("request.proposed", request("awaiting_confirmation", "请修复问题")));
    assert(confirmations == 0 && client.view.request.text_present && !client.view.request.capture_pending);
    const char *fields[] = {"request_id", "agent_session_id", "target_title"};
    for (unsigned i = 0; i < 3U; ++i) {
        cJSON *wrong = request("awaiting_confirmation", "原文");
        cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(wrong, fields[i]), "old");
        assert(!notification("request.proposed", wrong));
    }
    assert(notification("request.updated", request("running", "请修复问题")));
    assert(!linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_CANCEL, NULL, clock_now++));
    assert(notification("request.updated", request("completed", "请修复问题")));
    assert(notification("request.proposed", request("awaiting_confirmation", "请修复问题")));
    assert(client.view.request.status == LINX_GATEWAY_REQUEST_COMPLETED && confirmations == 0);
    assert(linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_PREPARE, NULL, clock_now++));
    assert(response(request("awaiting_text", NULL)));
    assert(linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_CANCEL, NULL, clock_now++));
    assert(!client.view.request.capture_pending && cancellations == 1);
    assert(notification("request.proposed", request("awaiting_confirmation", "取消期间原文")));
    assert(response(request("cancelled", "取消期间原文")));
    assert(client.view.request.status == LINX_GATEWAY_REQUEST_CANCELLED && confirmations == 0);
}

static void test_cancel_recovery(void)
{
    start(true); prepare();
    assert(linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_CANCEL, NULL, clock_now++));
    assert(linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_HISTORY, NULL, clock_now++));
    assert(response(request("awaiting_text", NULL))); assert(cancellations == 1);
    assert(!client.view.request.capture_pending);
    uint32_t expiry = client.pending.sent_at + LINX_GATEWAY_TIMEOUT_MS;
    linx_gateway_client_tick(&client, expiry - 1); assert(cancellations == 1);
    linx_gateway_client_tick(&client, expiry); assert(cancellations == 1 && strstr(sent, "request.get"));
    assert(response(request("awaiting_confirmation", "取消未确认")));
    assert(cancellations == 1 && !client.view.request.capture_pending);
    assert(response(page(0, true, NULL)));
    assert(linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_REQUEST_GET, NULL, clock_now++));
    assert(response(request("cancelled", "取消未确认")));
    /* prepare 丢应答仅 get，并且读取 awaiting_text 永远不开麦。 */
    assert(linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_PREPARE, NULL, clock_now++));
    unsigned count = preparations;
    linx_gateway_client_tick(&client, client.pending.sent_at + LINX_GATEWAY_TIMEOUT_MS);
    assert(strstr(sent, "request.get") && preparations == count);
    assert(response(request("awaiting_text", NULL))); assert(!linx_gateway_client_claim_capture(&client));
    assert(linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_CANCEL, NULL, clock_now++));
    assert(error_response("CONFLICT", "已经提交")); assert(strstr(sent, "request.get"));
    assert(response(request("awaiting_confirmation", "原文")));
}

static void test_request_snapshot_recovery(void)
{
    start(true); prepare(); assert(response(request("awaiting_text", NULL)));
    char text[16387];
    for (unsigned i = 0; i < 8193U; ++i) { text[i * 2U] = (char)0xc3; text[i * 2U + 1U] = (char)0xa9; }
    text[16384] = '\0';
    assert(notification("request.proposed", request("awaiting_confirmation", text)));
    assert(client.view.request.text_present);
    /* 16KiB原文只借助解析器校验，不留在client常驻存储。 */
    for (size_t i = 0; i + 48U <= sizeof(client); ++i) assert(memcmp((const char *)&client + i, text, 48U));
    text[16384] = (char)0xc3; text[16385] = (char)0xa9; text[16386] = '\0';
    assert(!notification("request.proposed", request("awaiting_confirmation", text)));
    assert(client.view.error == LINX_GATEWAY_ERROR_CAPACITY);
    text[16384] = '\0';
    linx_gateway_client_disconnected(&client, client.view.generation); assert(client.view.request.uncertain);
    linx_gateway_client_connected(&client, 4, "AA:BB:CC:DD:EE:FF", clock_now++);
    assert(response(parsed("{\"device_id\":\"AA:BB:CC:DD:EE:FF\",\"desktop_client_id\":\"desktop\"}")));
    assert(strstr(sent, "request.get"));
    assert(response(request("unknown", text)));
    assert(client.view.request.status == LINX_GATEWAY_REQUEST_UNKNOWN && confirmations == 0);
    assert(!linx_gateway_client_claim_capture(&client));
    assert(!linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_PREPARE, NULL, clock_now++));
    assert(response(page(0, false, NULL))); assert(response(session("session")));
    assert(linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_REQUEST_GET, NULL, clock_now++));
    assert(response(request("running", text))); assert(client.view.request.status == LINX_GATEWAY_REQUEST_RUNNING);
    assert(notification("request.updated", request("failed", text)));
    assert(linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_PREPARE, NULL, clock_now++));
    assert(!client.view.request.text_present);
}

static void test_capacity_and_pairing(void)
{
    start(true);
    assert(linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_REFRESH, NULL, clock_now++));
    assert(!response(page(6, false, NULL))); assert(client.view.session_count == 0);
    cJSON *value = page(2, false, NULL), *items = cJSON_GetObjectItemCaseSensitive(value, "items");
    cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(items, 1), "agent_session_id"), "item-0");
    assert(!response(value));
    value = page(1, false, NULL); items = cJSON_GetObjectItemCaseSensitive(value, "items");
    char large[1026]; memset(large, 'x', 1025); large[1025] = '\0';
    cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(items, 0), "title"), large);
    assert(!response(value)); assert(client.view.error == LINX_GATEWAY_ERROR_CAPACITY);
    assert(response(page(1, false, NULL))); assert(!response(session("wrong")));
    assert(response(session("item-0")));
    assert(linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_HISTORY, NULL, clock_now++));
    assert(!response(page(17, true, NULL)));
    value = page(2, true, NULL); items = cJSON_GetObjectItemCaseSensitive(value, "items");
    cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(items, 1), "message_id"), "item-0");
    assert(!response(value)); assert(response(page(0, true, NULL)));
    const char *unexpected = "{\"id\":\"x\",\"type\":\"response\",\"reply_to\":\"unknown\",\"payload\":{\"ok\":true,\"data\":{}}}";
    assert(!linx_gateway_client_receive(&client, 3, unexpected, strlen(unexpected), clock_now));
    assert(!linx_gateway_client_receive(&client, 3, "{}", 2, clock_now));
    assert(!linx_gateway_client_receive(&client, 3, "[", 1, clock_now));
    accept_send = false;
    assert(linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_REFRESH, NULL, clock_now++));
    assert(client.view.error == LINX_GATEWAY_ERROR_BUSY && !client.pending.operation);
    accept_send = true; linx_gateway_client_tick(&client, clock_now++);
    assert(response(page(0, false, NULL))); assert(response(session("item-0")));
    assert(linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_HISTORY, NULL, clock_now++));
    linx_gateway_client_tick(&client, client.pending.sent_at + LINX_GATEWAY_TIMEOUT_MS);
    assert(client.view.error == LINX_GATEWAY_ERROR_TIMEOUT && !client.pending.operation);
}

static void test_strict_json(void)
{
    const char *bad[] = { "", "{} garbage", "{\"a\":1,}", "[1,]", "[true false]", "[null", "[tru]",
        "[1.]", "[1e]", "[-]", "[01]", "[+1]", "[\"\\u0000\"]", "[\"\\ud800\"]", "[\"\\udc00\"]",
        "[\"\\uXXXX\"]", "[\"\\u12\"]", "[\"\\ud800\\u0061\"]", "[\"\\q\"]", "[\"a\n\"]", "[\"unterminated]",
        "[\"\xc0\x80\"]", "[\"\xed\xa0\x80\"]", "[\"\xf4\x90\x80\x80\"]", "[\"\xe4\xb8\"]", "[\"\xe4\x41\x80\"]" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        assert(!linx_gateway_client_validate_json("Id", bad[i], strlen(bad[i])));
    }
    assert(!linx_gateway_client_validate_json(NULL, "{}", 2));
    assert(!linx_gateway_client_validate_json("Id", NULL, 2));
    assert(!linx_gateway_client_validate_json("unknown", "{}", 2));
    assert(linx_gateway_client_validate_json("Id", "\"\\ud83d\\ude00\"", 14));
    assert(linx_gateway_client_validate_json("Id", "\"\\\"\\\\\\/\\b\\f\\n\\r\\t\"", 18));
    assert(!linx_gateway_client_validate_json("ContentChunk", "{\"text\":\"a\",\"text\":\"b\",\"next_offset\":null}", 45));
    const char *fractions[] = {"-1", "1.5", "1e999", "9007199254740992"};
    for (size_t i = 0; i < sizeof(fractions) / sizeof(fractions[0]); ++i) {
        char json[128]; snprintf(json, sizeof(json), "{\"text\":\"a\",\"next_offset\":%s}", fractions[i]);
        assert(!linx_gateway_client_validate_json("ContentChunk", json, strlen(json)));
    }
    char id[518]; id[0] = '"';
    for (unsigned i = 0; i < 128; ++i) { memcpy(id + 1 + i * 4, "\xf0\x9f\x98\x80", 4); }
    id[513] = '"'; id[514] = '\0';
    assert(linx_gateway_client_validate_json("Id", id, 514));
    memcpy(id + 513, "\xf0\x9f\x98\x80", 4); id[517] = '"';
    assert(!linx_gateway_client_validate_json("Id", id, 518));
}


static void test_state_matrix(void)
{
    const char *statuses[] = {"", "awaiting_text", "awaiting_confirmation", "sending", "running", "completed", "failed", "cancelled", "unknown"};
    const unsigned allowed[] = {0, (1U<<1)|(1U<<2)|(1U<<6)|(1U<<7),
        (1U<<2)|(1U<<3)|(1U<<4)|(1U<<5)|(1U<<6)|(1U<<7)|(1U<<8),
        (1U<<3)|(1U<<4)|(1U<<5)|(1U<<6)|(1U<<8), (1U<<4)|(1U<<5)|(1U<<6)|(1U<<8),
        1U<<5, 1U<<6, 1U<<7, (1U<<3)|(1U<<4)|(1U<<5)|(1U<<6)|(1U<<8)};
    for (unsigned from = 1; from <= 8; ++from) {
        for (unsigned to = 1; to <= 8; ++to) {
            start(true); prepare(); assert(response(request("awaiting_text", NULL)));
            if (from != 1) {
                assert(notification("request.proposed", request("awaiting_confirmation", "固定原文")));
                if (from != 2) { assert(notification("request.updated", request(statuses[from], "固定原文"))); }
            }
            assert(notification("request.updated", request(statuses[to], to == 1 ? NULL : "固定原文")));
            assert(client.view.request.status == (linx_gateway_request_status_t)((allowed[from] & (1U<<to)) ? to : from));
        }
    }
}

static long allocation_limit, allocation_calls, outstanding;
static void *failing_malloc(size_t size)
{
    if (allocation_calls++ == allocation_limit) { return NULL; }
    void *result = malloc(size); if (result) { ++outstanding; } return result;
}
static void failing_free(void *pointer) { if (pointer) { --outstanding; free(pointer); } }

static void test_allocation_failures(void)
{
    cJSON_Hooks hooks = {.malloc_fn = failing_malloc, .free_fn = failing_free};
    /* 每一个 cJSON 创建、字段、字符串、打印分配点；失败后模型不变且无泄漏。 */
    for (unsigned operation = 0; operation < 5; ++operation) {
        for (long point = 0; point < 45; ++point) {
            start(true); select_session("session");
            if (operation == 3) {
                assert(linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_PREPARE, NULL, clock_now++));
                assert(response(request("awaiting_text", NULL)));
                assert(notification("request.proposed", request("awaiting_confirmation", "原文")));
            }
            if (operation == 4) {
                assert(linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_REFRESH, NULL, clock_now++));
                assert(response(page(1, false, "next"))); assert(response(session("session")));
            }
            allocation_limit = point; allocation_calls = outstanding = 0;
            injecting_failure = true; cJSON_InitHooks(&hooks);
            if (operation == 0) { linx_gateway_client_connected(&client, 4, "AA:BB:CC:DD:EE:FF", clock_now++); }
            else {
                const linx_gateway_action_t actions[] = {LINX_GATEWAY_ACTION_REFRESH, LINX_GATEWAY_ACTION_CONTENT,
                    LINX_GATEWAY_ACTION_PREPARE, LINX_GATEWAY_ACTION_CANCEL, LINX_GATEWAY_ACTION_SESSIONS_NEXT};
                assert(linx_gateway_client_action(&client, actions[operation], operation == 1 ? "message" : NULL, clock_now++));
            }
            cJSON_InitHooks(NULL); injecting_failure = false;
            assert(outstanding == 0);
            assert(!strcmp(client.view.detail.title, "真实标题"));
        }
    }
    for (long point = 0; point < 80; ++point) {
        start(true); select_session("session");
        assert(linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_SELECT, "session", clock_now++));
        cJSON *root = cJSON_CreateObject(), *payload = cJSON_AddObjectToObject(root, "payload");
        cJSON_AddStringToObject(root, "id", "reply"); cJSON_AddStringToObject(root, "type", "response");
        cJSON_AddStringToObject(root, "reply_to", client.pending.id); cJSON_AddTrueToObject(payload, "ok");
        cJSON_AddItemToObject(payload, "data", session("session"));
        char *json = cJSON_PrintUnformatted(root); cJSON_Delete(root);
        allocation_limit = point; allocation_calls = outstanding = 0; cJSON_InitHooks(&hooks);
        bool ok = linx_gateway_client_receive(&client, 3, json, strlen(json), clock_now++);
        cJSON_InitHooks(NULL); free(json);
        assert(outstanding == 0);
        assert(!strcmp(client.view.detail.title, "真实标题"));
        if (!ok) { assert(client.view.error == LINX_GATEWAY_ERROR_MEMORY); }
    }
}

static void test_lexer_bounds(void)
{
    char bytes[65538];
    const char *valid[] = {" \r\n\t\"é\" ", "\"\\u3000\"", "\"\\u00a0\"", "\"\\u0085\"", "\"\\u1680\"", "\"\\u2000\"", "\"\\u2028\"", "\"\\u2029\"", "\"\\u202f\"", "\"\\u205f\""};
    assert(linx_gateway_client_validate_json("Id", valid[0], strlen(valid[0])));
    for (unsigned i = 1; i < sizeof(valid) / sizeof(valid[0]); ++i) {
        assert(!linx_gateway_client_validate_json("DraftText", valid[i], strlen(valid[i])));
    }
    const char *extra_bad[] = {"{x:1}", "{\"a\"1}", "{\"a}", "{\"a\":t}", "-", "1.5", "1E+2", "1e-2", "1E", "1e+", "\"\\", "\"\\u1", "\"\\ud800xabcde", "\"\\ud800\\x0000", "\"\\ud800\\uFFFF\"", "\"\\ud800\\uZZZZ\"", "\"\xe0\x80\x80\"", "\"\xf0\x80\x80\x80\"", "\"\x80\""};
    for (unsigned i = 0; i < sizeof(extra_bad) / sizeof(extra_bad[0]); ++i) {
        assert(!linx_gateway_client_validate_json("Id", extra_bad[i], strlen(extra_bad[i])));
    }
    memset(bytes, ' ', 65536); memcpy(bytes, "\"x\"", 3);
    assert(linx_gateway_client_validate_json("Id", bytes, 65536));
    assert(!linx_gateway_client_validate_json("Id", bytes, 65537));
    bytes[0] = '['; size_t length = 1;
    for (unsigned i = 0; i < 256; ++i) { bytes[length++] = '0'; bytes[length++] = ','; }
    bytes[length - 1] = ']';
    assert(!linx_gateway_client_validate_json("SessionPage", bytes, length));
    cJSON_Hooks hooks = {.malloc_fn = failing_malloc, .free_fn = failing_free};
    for (unsigned depth = 8; depth <= 9; ++depth) {
        for (unsigned i = 0; i < depth; ++i) { bytes[i] = '['; bytes[depth + 1 + i] = ']'; }
        bytes[depth] = '0'; allocation_limit = -1; allocation_calls = outstanding = 0;
        cJSON_InitHooks(&hooks);
        assert(!linx_gateway_client_validate_json("Id", bytes, depth * 2 + 1));
        cJSON_InitHooks(NULL); assert(outstanding == 0);
        assert((allocation_calls > 0) == (depth == 8));
    }
    for (unsigned nodes = 256; nodes <= 257; ++nodes) {
        length = 1; bytes[0] = '[';
        for (unsigned i = 1; i < nodes; ++i) { bytes[length++] = '0'; bytes[length++] = ','; }
        bytes[length - 1] = ']'; allocation_limit = -1; allocation_calls = outstanding = 0;
        cJSON_InitHooks(&hooks);
        assert(!linx_gateway_client_validate_json("Id", bytes, length));
        cJSON_InitHooks(NULL); assert(outstanding == 0);
        assert((allocation_calls > 0) == (nodes == 256));
    }
    assert(!linx_gateway_client_validate_json("DeviceId", "\"AABBCCDDEEFF\"", 14));
    assert(!linx_gateway_client_validate_json("DeviceId", "\"AA-BB-CC-DD-EE-FF\"", 19));
    assert(!linx_gateway_client_validate_json("MacAddress", "\"AA:BB-CC:DD:EE:FF\"", 19));
    assert(!linx_gateway_client_validate_json("MacAddress", "\"AG:BB:CC:DD:EE:FF\"", 19));
    assert(!linx_gateway_client_validate_json("device.hello", "{\"id\":\"x\",\"id\":\"y\",\"type\":\"device.hello\",\"payload\":{\"mac_address\":\"AABBCCDDEEFF\"}}", 95));
}


static void test_interleaving_and_limits(void)
{
    start(true);
    assert(!linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_REQUEST_GET, NULL, clock_now++));
    assert(!linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_CANCEL, NULL, clock_now++));
    assert(!linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_PREPARE, NULL, clock_now++));
    assert(linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_SELECT, "old", clock_now++));
    assert(linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_SELECT, "new", clock_now++));
    assert(response(session("old"))); assert(!client.view.detail_valid);
    assert(response(session("new"))); assert(!strcmp(client.view.detail.agent_session_id, "new"));
    assert(linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_HISTORY, "old", clock_now++));
    assert(linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_HISTORY, "new", clock_now++));
    assert(response(page(2, true, NULL))); assert(client.view.message_count == 0);
    assert(response(page(1, true, "next"))); assert(client.view.message_count == 1);
    assert(linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_HISTORY_NEXT, NULL, clock_now++));
    assert(error_response("CONFLICT", "游标失效")); assert(!*client.pending.cursor);
    assert(response(page(0, true, NULL)));
    assert(linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_CONTENT, "old-message", clock_now++));
    assert(linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_CONTENT, "new-message", clock_now++));
    assert(response(chunk("old", -1))); assert(!client.view.content_valid);
    assert(response(chunk("new", 3))); assert(!strcmp(client.view.content_message_id, "new-message"));
    assert(!linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_CONTENT_PREVIOUS, NULL, clock_now++));
    assert(linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_CONTENT_NEXT, NULL, clock_now++));
    cJSON *value = chunk("bad", -1); cJSON_ReplaceItemInObjectCaseSensitive(value, "next_offset", cJSON_CreateNumber(999999999999.0));
    assert(!response(value));
    assert(!response(chunk("跳字", 999))); assert(!strcmp(client.view.content, "new"));
    assert(response(chunk("中文字", 12))); assert(client.view.content_offset == 3 && client.view.content_next_offset == 12);
    assert(notification("message.available", parsed("{\"agent_session_id\":\"new\",\"message_id\":\"newer\"}")));
    assert(response(session("new"))); assert(response(page(0, true, NULL)));
    assert(client.view.new_reply && !strcmp(client.view.content, "中文字"));
    assert(linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_CONTENT_NEXT, NULL, clock_now++));
    assert(linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_SELECT, "other", clock_now++));
    assert(response(chunk("stale", -1))); assert(!strcmp(client.view.content, "中文字"));
    assert(response(session("other")));
    char large[18001];
    for (unsigned i = 0; i < 6000; ++i) { memcpy(large + i * 3, "中", 3); }
    large[18000] = '\0';
    assert(linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_PREPARE, "other", clock_now++));
    assert(response(request("awaiting_text", NULL)));
    assert(!notification("request.proposed", request("awaiting_confirmation", large)));
    assert(client.view.error == LINX_GATEWAY_ERROR_CAPACITY && !client.view.request.text_present);
    value = request("failed", NULL);
    cJSON *error = parsed("{\"code\":\"INTERNAL\",\"message\":\"x\"}");
    cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(error, "message"), large);
    cJSON_ReplaceItemInObjectCaseSensitive(value, "error", error);
    assert(!notification("request.updated", value)); assert(client.view.request.status == LINX_GATEWAY_REQUEST_AWAITING_TEXT);
    value = request("awaiting_text", NULL);
    cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(value, "agent_session_id"), "wrong");
    assert(!notification("request.updated", value));
    value = request("awaiting_text", NULL);
    cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(value, "target_title"), "changed");
    assert(!notification("request.updated", value));
    assert(notification("request.proposed", request("awaiting_confirmation", "原文")));
    accept_send = false;
    assert(linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_CANCEL, NULL, clock_now++));
    assert(!client.view.request.capture_pending);
    accept_send = true; linx_gateway_client_tick(&client, clock_now++);
    assert(strstr(sent, "request.cancel"));
    assert(response(request("awaiting_confirmation", "原文"))); assert(confirmations == 0);
    assert(linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_REQUEST_GET, NULL, clock_now++));
    linx_gateway_client_tick(&client, client.pending.sent_at + LINX_GATEWAY_TIMEOUT_MS);
    unsigned before = send_count; linx_gateway_client_tick(&client, clock_now++); assert(send_count == before);
    /* 发送编号耗尽显式失败，绝不回绕复用旧 packet/request ID。 */
    start(true); prepare();
    value = request("awaiting_text", NULL);
    cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(value, "target_title"), "服务端登记的新标题");
    assert(response(value)); assert(!strcmp(client.view.request.target_title, "服务端登记的新标题"));
    value = request("awaiting_confirmation", "原文");
    cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(value, "target_title"), "又一次改名");
    assert(!notification("request.proposed", value));
    start(true); select_session("session"); client.sequence = UINT32_MAX;
    assert(!linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_PREPARE, NULL, clock_now++));
    assert(!client.view.request.valid);
    assert(linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_REFRESH, NULL, clock_now++));
    assert(client.view.error == LINX_GATEWAY_ERROR_CAPACITY);
}


static void test_boot_and_early_snapshot(void)
{
    linx_gateway_client_config_t config = {.configured = true, .instance_id = 1, .send = send_json, .user = &client};
    linx_gateway_client_deinit(&client); linx_gateway_client_init(&client, &config); linx_gateway_client_connected(&client, 1, "AABBCCDDEEFF", clock_now++);
    char first_id[64]; strcpy(first_id, client.pending.id);
    config.instance_id = 2;
    linx_gateway_client_deinit(&client); linx_gateway_client_init(&client, &config); linx_gateway_client_connected(&client, 1, "AABBCCDDEEFF", clock_now++);
    assert(strcmp(first_id, client.pending.id) && strlen(client.pending.id) < sizeof(first_id));
    config.instance_id = 0; linx_gateway_client_deinit(&client); linx_gateway_client_init(&client, &config); assert(!client.view.configured);
    config.configured = false; linx_gateway_client_deinit(&client); linx_gateway_client_init(&client, &config); assert(!client.view.configured);
    start(true); prepare();
    cJSON *value = request("awaiting_confirmation", "抢先到达的最终原文");
    cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(value, "target_title"), "首快照标题");
    assert(notification("request.proposed", value));
    assert(!strcmp(client.view.request.target_title, "首快照标题") && !client.view.request.capture_pending);
    assert(response(request("awaiting_text", NULL)));
    assert(client.view.request.status == LINX_GATEWAY_REQUEST_AWAITING_CONFIRMATION && !linx_gateway_client_claim_capture(&client));
    assert(notification("request.updated", request("completed", "抢先到达的最终原文")));
    assert(!client.pending.operation); /* 终态快照已证明结果，不再等旧请求应答而假报超时。 */
    linx_gateway_client_tick(&client, clock_now + LINX_GATEWAY_TIMEOUT_MS);
    assert(!client.view.request.uncertain);
    assert(linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_PREPARE, NULL, clock_now++));
    assert(response(request("awaiting_text", NULL)));
    assert(notification("request.proposed", request("awaiting_confirmation", "确认原文")));
    accept_send = false;
    unsigned count = confirmations;
    linx_gateway_client_disconnected(&client, client.view.generation);
    accept_send = true; linx_gateway_client_connected(&client, 4, "AABBCCDDEEFF", clock_now++);
    assert(response(parsed("{\"device_id\":\"AA:BB:CC:DD:EE:FF\",\"desktop_client_id\":\"desktop\"}")));
    assert(response(request("awaiting_confirmation", "确认原文"))); assert(confirmations == count);
    start(true); select_session("session"); accept_send = false;
    assert(linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_PREPARE, NULL, clock_now++));
    assert(!client.prepare_sent);
    assert(linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_CANCEL, NULL, clock_now++));
    assert(!client.view.request.valid); /* 尚未进入发送槽的本地意图可撤销。 */
    assert(linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_PREPARE, NULL, clock_now++));
    linx_gateway_client_disconnected(&client, client.view.generation);
    assert(!client.view.request.valid); /* 断线不能自动把未发送意图变成新prepare。 */
    accept_send = true;
}


static void test_rejected_prepare_release(void)
{
    const char *rejections[] = {"NOT_FOUND", "INVALID_ARGUMENT", "UNAUTHENTICATED", "PERMISSION_DENIED"};
    for (unsigned i = 0; i < sizeof(rejections) / sizeof(rejections[0]); ++i) {
        start(true); prepare();
        char old_id[LINX_GATEWAY_ID_CAPACITY]; strcpy(old_id, client.view.request.request_id);
        assert(error_response(rejections[i], "明确拒绝创建"));
        assert(client.prepare_rejected && strstr(sent, "request.get") && client.view.request.valid);
        assert(error_response("NOT_FOUND", "请求尚未创建"));
        assert(!client.view.request.valid && !client.view.request.uncertain && !client.recovery_needed);
        assert(client.view.request.status == LINX_GATEWAY_REQUEST_NONE);
        assert(!strcmp(client.view.request.error_code, rejections[i]));
        assert(!strcmp(client.view.request.error_message, "明确拒绝创建"));
        unsigned count = preparations;
        linx_gateway_client_tick(&client, clock_now++); assert(preparations == count);
        assert(linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_PREPARE, NULL, clock_now++));
        assert(preparations == count + 1 && strcmp(old_id, client.view.request.request_id));
        assert(!client.view.request.error_message[0] && !client.prepare_rejected);
    }
    const char *uncertain[] = {"CONFLICT", "INTERNAL", "DEPENDENCY_UNAVAILABLE", "RESOURCE_EXHAUSTED"};
    for (unsigned i = 0; i < sizeof(uncertain) / sizeof(uncertain[0]); ++i) {
        start(true); prepare(); assert(error_response(uncertain[i], "创建结果未确定"));
        assert(error_response("NOT_FOUND", "暂未找到请求"));
        assert(client.view.request.valid && client.view.request.uncertain);
        assert(!linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_PREPARE, NULL, clock_now++));
        assert(preparations == 1);
    }
    start(true); prepare();
    linx_gateway_client_tick(&client, client.pending.sent_at + LINX_GATEWAY_TIMEOUT_MS);
    assert(error_response("NOT_FOUND", "暂未找到请求"));
    assert(client.view.request.valid && client.view.request.uncertain && !client.prepare_rejected);
    assert(!linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_PREPARE, NULL, clock_now++));
    start(true); prepare(); assert(error_response("NOT_FOUND", "明确拒绝创建"));
    assert(notification("request.proposed", request("awaiting_confirmation", "服务端已存在")));
    assert(!client.prepare_rejected); assert(error_response("NOT_FOUND", "快照缺失"));
    assert(client.view.request.valid);
    start(true); prepare(); assert(response(request("awaiting_text", NULL)));
    assert(notification("request.proposed", request("awaiting_confirmation", "原文")));
    linx_gateway_client_disconnected(&client, client.view.generation);
    linx_gateway_client_connected(&client, 4U, "AABBCCDDEEFF", clock_now++);
    assert(response(parsed("{\"device_id\":\"AA:BB:CC:DD:EE:FF\",\"desktop_client_id\":\"desktop\"}")));
    assert(error_response("NOT_FOUND", "结果不可查"));
    assert(client.view.request.valid && client.view.request.uncertain);
    assert(!linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_PREPARE, NULL, clock_now++));
    assert(confirmations == 0);
}

static void test_history_owner_failure(void)
{
    start(true); select_session("old-session");
    assert(linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_HISTORY, NULL, clock_now++));
    assert(response(page(1, true, "old-cursor")));
    assert(linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_HISTORY_NEXT, NULL, clock_now++));
    assert(response(page(1, true, "next-old-cursor")));
    assert(client.history_depth == 1);
    assert(linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_HISTORY, NULL, clock_now++));
    assert(error_response("INTERNAL", "当前会话历史读取失败"));
    assert(client.view.message_count == 1 && !strcmp(client.view.messages[0].message_id, "item-0"));
    assert(!strcmp(client.view.history_session_id, "old-session"));
    select_session("new-session");
    assert(linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_HISTORY, NULL, clock_now++));
    assert(client.view.message_count == 0 && !client.view.history_next_cursor[0] && client.history_depth == 0);
    assert(!strcmp(client.view.history_session_id, "new-session"));
    assert(error_response("NOT_FOUND", "新会话不存在"));
    assert(client.view.message_count == 0 && !client.view.history_next_cursor[0]);
    assert(!linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_HISTORY_NEXT, NULL, clock_now++));
    assert(!linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_HISTORY_PREVIOUS, NULL, clock_now++));
    assert(!strstr(sent, "message.content.get"));
    assert(linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_HISTORY, NULL, clock_now++));
    assert(response(page(1, true, NULL))); assert(client.view.message_count == 1);
}

static void test_content_pending_and_transport_error(void)
{
    assert(!linx_gateway_client_content_pending(NULL));
    linx_gateway_client_transport_error(NULL, 0);
    linx_gateway_client_deinit(&client); linx_gateway_client_init(&client, NULL); linx_gateway_client_transport_error(&client, 0);
    assert(client.view.error == LINX_GATEWAY_ERROR_UNCONFIGURED);
    linx_gateway_client_config_t config = {.configured = true, .instance_id = 123, .send = send_json, .user = &client};
    linx_gateway_client_deinit(&client); linx_gateway_client_init(&client, &config);
    uint32_t revision = client.view.revision;
    linx_gateway_client_transport_error(&client, 1); assert(client.view.revision == revision);
    linx_gateway_client_transport_error(&client, 0);
    assert(client.view.error == LINX_GATEWAY_ERROR_TRANSPORT && client.view.revision > revision);
    assert(!client.view.connected && !client.view.request.valid && client.view.error_message[0]);
    linx_gateway_client_connected(&client, 3, "AABBCCDDEEFF", clock_now++);
    assert(client.view.error == LINX_GATEWAY_ERROR_NONE);
    assert(response(parsed("{\"device_id\":\"AA:BB:CC:DD:EE:FF\",\"desktop_client_id\":\"desktop\"}")));
    assert(response(page(0, false, NULL))); select_session("session");
    assert(!linx_gateway_client_content_pending(&client));
    assert(linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_CONTENT, "message", clock_now++));
    assert(linx_gateway_client_content_pending(&client)); assert(response(chunk("首块", 6)));
    assert(!linx_gateway_client_content_pending(&client));
    assert(linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_REFRESH, NULL, clock_now++));
    assert(client.view.loading && !linx_gateway_client_content_pending(&client));
    assert(linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_CONTENT_NEXT, NULL, clock_now++));
    assert(linx_gateway_client_content_pending(&client) && client.view.content_offset == 0);
    assert(response(page(0, false, NULL)));
    assert(linx_gateway_client_content_pending(&client) && client.pending.offset == 6);
    assert(response(chunk("末块", -1))); assert(client.view.content_offset == 6);
    assert(!linx_gateway_client_content_pending(&client)); assert(response(session("session")));
    prepare(); assert(response(request("awaiting_text", NULL)));
    assert(notification("request.proposed", request("awaiting_confirmation", "原文")));
    revision = client.view.revision;
    linx_gateway_client_transport_error(&client, 2); assert(client.view.revision == revision && client.view.connected);
    linx_gateway_client_transport_error(&client, 3);
    assert(client.view.error == LINX_GATEWAY_ERROR_TRANSPORT && !client.view.connected);
    assert(client.view.request.valid && client.view.request.uncertain);
    assert(client.view.request.status == LINX_GATEWAY_REQUEST_AWAITING_CONFIRMATION);
}








static void test_fixtures(const char *path)
{
    FILE *file = fopen(path, "rb"); assert(file);
    assert(fseek(file, 0, SEEK_END) == 0); long length = ftell(file); rewind(file);
    char *bytes = malloc((size_t)length + 1); assert(bytes);
    assert(fread(bytes, 1, (size_t)length, file) == (size_t)length); fclose(file); bytes[length] = '\0';
    cJSON *fixtures = cJSON_Parse(bytes); free(bytes); assert(fixtures);
    for (const cJSON *item = fixtures->child; item; item = item->next) {
        const char *schema = cJSON_GetObjectItemCaseSensitive(item, "schema")->valuestring;
        if (strncmp(schema, "common.schema.json#/$defs/", 26) && strncmp(schema, "firmware-gateway.schema.json#/$defs/", 36)) { continue; }
        const char *definition = strstr(schema, "/$defs/") + 7;
        char *json = cJSON_PrintUnformatted(cJSON_GetObjectItemCaseSensitive(item, "data")); assert(json);
        bool valid = linx_gateway_client_validate_json(definition, json, strlen(json));
        bool expected = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item, "valid"));
        if (valid != expected) { fprintf(stderr, "fixture: %s\n", cJSON_GetObjectItemCaseSensitive(item, "name")->valuestring); }
        assert(valid == expected); cJSON_free(json);
    }
    cJSON_Delete(fixtures);
}






static void test_formal_page_profile(void)
{
    start(true);
    assert(linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_REFRESH, NULL, clock_now++));
    assert(response(page(5, false, "next-formal-page")));
    assert(client.view.session_count == 2U);
    assert(!strcmp(client.view.sessions_next_cursor, "next-formal-page"));
    assert(response(session(client.view.selected_session_id)));
    assert(linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_HISTORY, "session", clock_now++));
    cJSON *history = cJSON_CreateObject(), *items = cJSON_AddArrayToObject(history, "items");
    cJSON_AddStringToObject(history, "next_cursor", "older-formal-page");
    for (unsigned i = 0; i < 16U; ++i) {
        char id[24]; snprintf(id, sizeof(id), "message-%u", i);
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "message_id", id);
        cJSON_AddStringToObject(item, "role", i % 2U ? "assistant" : "user");
        cJSON_AddStringToObject(item, "summary", "完整正式消息摘要");
        cJSON_AddItemToArray(items, item);
    }
    assert(response(history)); assert(client.view.message_count == 2U);
    assert(!strcmp(client.view.messages[0].message_id, "message-0"));
    assert(!strcmp(client.view.messages[1].message_id, "message-1"));
    assert(!strcmp(client.view.history_next_cursor, "older-formal-page"));
}

static void test_formal_tail_validation_and_transient_drop(void)
{
    start(true); assert(linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_REFRESH, NULL, clock_now++));
    cJSON *value = page(5, false, "next"), *items = cJSON_GetObjectItemCaseSensitive(value, "items");
    char summary[8193], title[1301]; memset(summary, 'S', sizeof(summary) - 1U); summary[sizeof(summary) - 1U] = '\0';
    memset(title, 'T', sizeof(title) - 1U); title[sizeof(title) - 1U] = '\0';
    cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(items, 0), "summary"), summary);
    cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(items, 4), "title"), title);
    assert(response(value)); /* 未缓存标题不占本地容量；正式schema仍完整验证。 */
    assert(client.view.sessions_omitted && client.view.session_count == 2U);
    for (size_t i = 0; i + 48U <= sizeof(client); ++i) assert(memcmp((const char *)&client + i, summary, 48U));
    assert(response(session("item-0")));
    assert(linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_REFRESH, NULL, clock_now++));
    value = page(5, false, "bad"); items = cJSON_GetObjectItemCaseSensitive(value, "items");
    cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(items, 4), "status"), "waiting");
    assert(!response(value)); assert(!strcmp(client.view.sessions_next_cursor, "next"));
    value = page(5, false, "bad"); items = cJSON_GetObjectItemCaseSensitive(value, "items");
    cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(items, 4), "agent_session_id"), "item-0");
    assert(!response(value)); assert(client.view.session_count == 2U);
    assert(response(page(2, false, NULL))); assert(!client.view.sessions_omitted); assert(response(session("item-0")));
    assert(linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_HISTORY, NULL, clock_now++));
    value = page(16, true, "bad"); items = cJSON_GetObjectItemCaseSensitive(value, "items");
    char invalid[122]; memset(invalid, 'x', 121U); invalid[121] = '\0';
    cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(items, 15), "summary"), invalid);
    assert(!response(value) && !client.view.message_count);
    value = page(16, true, "bad"); items = cJSON_GetObjectItemCaseSensitive(value, "items");
    cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(items, 15), "message_id"), "item-0");
    assert(!response(value) && !client.view.message_count);
    assert(response(page(16, true, "older"))); assert(client.view.messages_omitted);
    linx_gateway_client_drop_transient(NULL, true);
    linx_gateway_client_drop_transient(&client, false); assert(client.view.message_count == 2U);
    assert(linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_CONTENT, "item-0", clock_now++));
    assert(response(chunk("正文", 6)));
    assert(linx_gateway_client_action(&client, LINX_GATEWAY_ACTION_CONTENT_NEXT, NULL, clock_now++));
    linx_gateway_client_drop_transient(&client, true);
    assert(!client.view.message_count && !client.view.messages_omitted && !client.view.content_valid);
    assert(!client.pending.operation && !client.intent.operation && !client.view.content[0]);
    assert(!response(chunk("迟到正文", -1)) && !client.view.content_valid);
    assert(client.view.detail_valid && !strcmp(client.view.selected_session_id, "item-0"));
}

static void test_borrowed_frame_boundaries(void)
{
    const char guards[] = {'X', '}', '\0', (char)0xff};
    for (size_t i = 0; i < sizeof(guards); ++i) {
        char input[] = {'"', 'x', '"', guards[i]};
        assert(linx_gateway_client_validate_json("Id", input, 3U));
        assert(input[0] == '"' && input[3] == guards[i]);
    }
    const char whitespace[] = {' ', '\t', '\r', '\n'};
    for (size_t i = 0; i < sizeof(whitespace); ++i) {
        char input[] = {'"', 'x', '"', whitespace[i], '!'};
        assert(linx_gateway_client_validate_json("Id", input, 4U));
        assert(!linx_gateway_client_validate_json("Id", input, 5U));
    }
    const char combined[] = {'"', 'x', '"', ' ', '\t', '\r', '\n'};
    assert(linx_gateway_client_validate_json("Id", combined, sizeof(combined)));
    const char *invalid[] = {"\"x\"{}", "\"x\"true", "\"x\"!", "\"x\"\v", "\"x\"\f", "\"bad\x01\"", "\"\\u0000\""};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
        assert(!linx_gateway_client_validate_json("Id", invalid[i], strlen(invalid[i])));
    const char nul[] = {'"', 'x', '"', 0, '!'};
    assert(!linx_gateway_client_validate_json("Id", nul, sizeof(nul)));
    const char *page_json = "{\"items\":[],\"next_cursor\":null}";
    size_t size = strlen(page_json); char *exact = malloc(size); assert(exact); memcpy(exact, page_json, size);
    assert(linx_gateway_client_validate_json("SessionPage", exact, size));
    for (size_t i = 0; i < size; ++i) assert(!linx_gateway_client_validate_json("SessionPage", exact, i));
    free(exact);
}

static void test_borrowed_frame_heap_budget(void)
{
    start(true); prepare(); assert(response(request("awaiting_text", NULL)));
    char text[16385];
    for (size_t i = 0; i < 8192U; ++i) { text[i * 2U] = (char)0xc3; text[i * 2U + 1U] = (char)0xa9; }
    text[16384] = '\0';
    cJSON *root = cJSON_CreateObject(); cJSON_AddStringToObject(root, "id", "budget");
    cJSON_AddStringToObject(root, "type", "request.proposed");
    cJSON_AddItemToObject(root, "payload", request("awaiting_confirmation", text));
    char *json = cJSON_PrintUnformatted(root); assert(json); cJSON_Delete(root);
    size_t size = strlen(json); char *frame = malloc(size); assert(frame); memcpy(frame, json, size); cJSON_free(json);
    budget_begin(-1);
    bool ok = linx_gateway_client_receive(&client, client.view.generation, frame, size, clock_now++);
    size_t allocations = budget_calls;
    printf("borrowed frame heap: frame=%zu peak=%zu denied=%zu budget=%zu\n", size, budget_peak, budget_denied, budget_limit);
    budget_end(); assert(ok && client.view.request.text_present);
    assert(client.view.request.status == LINX_GATEWAY_REQUEST_AWAITING_CONFIRMATION);
    assert(budget_peak < budget_limit && !budget_denied);
    for (size_t fail_at = 0; fail_at < allocations; ++fail_at) {
        start(true); prepare(); assert(response(request("awaiting_text", NULL)));
        const linx_gateway_request_t before = client.view.request;
        budget_begin((long)fail_at);
        ok = linx_gateway_client_receive(&client, client.view.generation, frame, size, clock_now++);
        budget_end(); assert(!ok && client.view.error == LINX_GATEWAY_ERROR_MEMORY);
        assert(!memcmp(&before, &client.view.request, sizeof(before)));
    }
    free(frame);
}

int main(int argc, char **argv)
{
    test_borrowed_frame_boundaries();
    test_borrowed_frame_heap_budget();
    test_formal_page_profile();
    test_formal_tail_validation_and_transient_drop();
    test_binding_and_errors(); test_pages_and_notifications(); test_content();
    test_mcp_snapshot_merge(); test_cancel_recovery(); test_request_snapshot_recovery();
    test_capacity_and_pairing(); test_strict_json(); test_state_matrix(); test_allocation_failures(); test_lexer_bounds(); test_interleaving_and_limits(); test_boot_and_early_snapshot(); test_rejected_prepare_release(); test_history_owner_failure(); test_content_pending_and_transport_error();
    linx_gateway_client_deinit(&client);
    if (argc > 1) { test_fixtures(argv[1]); }
    if (argc > 2) { test_fixtures(argv[2]); }
    printf("gateway client tests passed; client=%zu view=%zu request=%zu bytes\n", sizeof(client), sizeof(client.view), sizeof(client.view.request));
    return 0;
}
