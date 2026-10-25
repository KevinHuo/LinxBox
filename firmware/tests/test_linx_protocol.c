#include "linx_protocol.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"

static const linx_device_info_t DEVICE = {
    .device_id = "D4:06:06:B6:A9:FA",
    .client_id = "550e8400-e29b-41d4-a716-446655440000",
    .version = "linx-voice-0.3.0",
    .elf_sha256 = "0123456789abcdef",
    .ssid = "测试\"Wi-Fi\\",
    .rssi = -53,
};
static const linx_reply_context_t CONTEXT = {.task_id = 3, .source_turn_id = 7, .image_id = UINT32_MAX};

static const cJSON *field(const cJSON *object, const char *name)
{
    const cJSON *result = cJSON_GetObjectItemCaseSensitive(object, name);
    assert(result != NULL);
    return result;
}

static void expect_string(const cJSON *object, const char *name, const char *expected)
{
    const cJSON *value = field(object, name);
    assert(cJSON_IsString(value));
    assert(strcmp(value->valuestring, expected) == 0);
}

static void test_builders(void)
{
    char output[4096];
    assert(linx_build_ota(output, sizeof(output), &DEVICE));
    cJSON *root = cJSON_Parse(output);
    assert(root != NULL);
    expect_string(root, "mac_address", DEVICE.device_id);
    expect_string(root, "uuid", DEVICE.client_id);
    expect_string(field(root, "application"), "version", DEVICE.version);
    expect_string(field(root, "application"), "elf_sha256", DEVICE.elf_sha256);
    expect_string(field(root, "board"), "type", "folotoy-ai-passport");
    expect_string(field(root, "board"), "ssid", DEVICE.ssid);
    assert(field(field(root, "board"), "rssi")->valueint == -53);
    cJSON_Delete(root);
    assert(linx_build_hello(output, sizeof(output), &CONTEXT));
    root = cJSON_Parse(output);
    assert(root != NULL);
    expect_string(root, "type", "hello");
    expect_string(root, "transport", "websocket");
    assert(field(root, "version")->valueint == 1);
    assert(cJSON_GetObjectItemCaseSensitive(root, "features") == NULL);
    const cJSON *audio = field(root, "audio_params");
    expect_string(audio, "format", "pcm");
    expect_string(audio, "endianness", "little");
    expect_string(audio, "sample_format", "signed_int16");
    assert(field(audio, "sample_rate")->valueint == 16000);
    assert(field(audio, "channels")->valueint == 1);
    assert(field(audio, "frame_duration")->valueint == 20);
    assert(field(audio, "frame_size")->valueint == 320);
    assert(field(audio, "bit_depth")->valueint == 16);
    const cJSON *replace = field(field(root, "agent_params"), "custom_replace_prompt");
    expect_string(replace, "linx_task_id", "3");
    expect_string(replace, "linx_turn_id", "7");
    expect_string(replace, "linx_image_id", "4294967295");
    cJSON_Delete(root);
    assert(linx_build_listen(output, sizeof(output), "session\"\\id", true));
    root = cJSON_Parse(output);
    expect_string(root, "type", "listen");
    expect_string(root, "session_id", "session\"\\id");
    expect_string(root, "state", "start");
    expect_string(root, "mode", "manual");
    cJSON_Delete(root);
    assert(linx_build_listen(output, sizeof(output), "", false));
    root = cJSON_Parse(output);
    expect_string(root, "state", "stop");
    expect_string(root, "session_id", "");
    assert(cJSON_GetObjectItemCaseSensitive(root, "mode") == NULL);
    cJSON_Delete(root);
    assert(linx_build_abort(output, sizeof(output), "session"));
    root = cJSON_Parse(output);
    expect_string(root, "type", "abort");
    expect_string(root, "session_id", "session");
    assert(cJSON_GetObjectItemCaseSensitive(root, "reason") == NULL);
    cJSON_Delete(root);
    assert(linx_build_abort(output, SIZE_MAX, ""));
    assert(!linx_build_abort(NULL, sizeof(output), ""));
    assert(!linx_build_abort(output, 0, ""));
    assert(!linx_build_abort(output, sizeof(output), NULL));
    assert(!linx_build_abort(output, sizeof(output), "\x7f"));
    assert(!linx_build_abort(output, sizeof(output), "bad\r\n"));
    char long_text[180];
    memset(long_text, 'a', sizeof(long_text) - 1);
    long_text[sizeof(long_text) - 1] = '\0';
    assert(!linx_build_abort(output, sizeof(output), long_text));
    assert(!linx_build_listen(output, 1, "", true));
    assert(output[0] == '\0');
    assert(!linx_build_hello(output, sizeof(output), NULL));
    assert(!linx_build_hello(NULL, sizeof(output), &CONTEXT));
    assert(!linx_build_hello(output, 2, &CONTEXT));
    assert(!linx_build_ota(output, sizeof(output), NULL));
    assert(!linx_build_ota(NULL, sizeof(output), &DEVICE));
    assert(!linx_build_ota(output, 2, &DEVICE));
    for (size_t i = 0; i < 5; ++i) {
        linx_device_info_t device = DEVICE;
        const char **members[] = {&device.device_id, &device.client_id, &device.version, &device.elf_sha256, &device.ssid};
        *members[i] = NULL;
        assert(!linx_build_ota(output, sizeof(output), &device));
        *members[i] = long_text;
        assert(!linx_build_ota(output, sizeof(output), &device));
        *members[i] = i == 4 ? "" : "\n";
        assert(linx_build_ota(output, sizeof(output), &device) == (i == 4));
        *members[i] = "";
        assert(linx_build_ota(output, sizeof(output), &device) == (i == 4));
    }
}

static void test_endpoints(void)
{
    linx_endpoint_t endpoint;
    assert(linx_parse_endpoint("wss://xrobo-io.qiniuapi.com/v1/ws/", &endpoint));
    assert(strcmp(endpoint.host, "xrobo-io.qiniuapi.com") == 0);
    assert(strcmp(endpoint.path, "/v1/ws/") == 0);
    assert(endpoint.port == 443);
    assert(linx_parse_endpoint("wss://EXAMPLE.net:65535/sock?q=1", &endpoint));
    assert(endpoint.port == 65535 && strcmp(endpoint.path, "/sock?q=1") == 0);
    assert(linx_parse_endpoint("wss://127.0.0.1:1", &endpoint));
    assert(endpoint.port == 1 && strcmp(endpoint.path, "/") == 0);
    assert(linx_parse_endpoint("wss://host?x=1", &endpoint));
    assert(strcmp(endpoint.path, "/?x=1") == 0);
    const char *const invalid[] = {NULL, "", "ws://host/", "https://host/", "wss:///x", "wss://:443/x",
        "wss://user@host/", "wss://host/#part", "wss://host:0/", "wss://host:65536/",
        "wss://host:99999999999999999999/", "wss://host:/", "wss://host:abc/", "wss://host:443z/",
        "wss://host name/", "wss://host\r\n/x", "wss://[::1]/", "wss://本机/x", "wss://host\\bad/"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) assert(!linx_parse_endpoint(invalid[i], &endpoint));
    assert(!linx_parse_endpoint("wss://host/", NULL));
    char host[180];
    strcpy(host, "wss://");
    memset(host + 6, 'a', 128);
    strcpy(host + 134, "/");
    assert(!linx_parse_endpoint(host, &endpoint));
    char oversized[300];
    memset(oversized, 'a', sizeof(oversized) - 1);
    memcpy(oversized, "wss://host/", 11);
    oversized[sizeof(oversized) - 1] = '\0';
    assert(!linx_parse_endpoint(oversized, &endpoint));

    const char *ws_url = "ws://124.220.162.10:8787/v1/devices/connect";
    assert(!linx_parse_endpoint(ws_url, &endpoint));
    assert(!linx_parse_gateway_endpoint(ws_url, false, &endpoint));
    assert(linx_parse_gateway_endpoint(ws_url, true, &endpoint));
    assert(!endpoint.tls && endpoint.port == 8787);
    assert(!strcmp(endpoint.host, "124.220.162.10") && !strcmp(endpoint.path, "/v1/devices/connect"));
    assert(linx_parse_gateway_endpoint("ws://host?x=1", true, &endpoint));
    assert(!endpoint.tls && endpoint.port == 80 && !strcmp(endpoint.path, "/?x=1"));
    assert(linx_parse_gateway_endpoint("ws://host", true, &endpoint));
    assert(!strcmp(endpoint.path, "/"));
    assert(linx_parse_gateway_endpoint("wss://host/", true, &endpoint));
    assert(endpoint.tls && endpoint.port == 443);
    assert(linx_parse_gateway_endpoint("wss://host/", false, &endpoint));
    assert(endpoint.tls);
    const char *const invalid_gateway[] = {NULL, "", "https://host/", "ws:///x", "ws://user@host/",
        "ws://host/#part", "ws://host:0/", "ws://host:65536/", "ws://host:/", "ws://host:abc/",
        "ws://host name/", "ws://host\r\n/x", "ws://[::1]/", "ws://host\\bad/"};
    for (size_t i = 0; i < sizeof(invalid_gateway) / sizeof(invalid_gateway[0]); ++i) {
        assert(!linx_parse_gateway_endpoint(invalid_gateway[i], true, &endpoint));
    }
    assert(!linx_parse_gateway_endpoint(ws_url, true, NULL));
}

static linx_gateway_result_t gateway(const char *json, linx_gateway_t *out)
{
    return linx_parse_gateway(json, strlen(json), out);
}

static void test_gateway(void)
{
    linx_gateway_t out;
    assert(gateway("{\"activation\":{\"code\":\"012345\"},\"websocket\":{\"url\":\"ws://xrobo-io.qiniuapi.com/v1/ws/\"}}", &out) == LINX_GATEWAY_BINDING);
    assert(strcmp(out.activation_code, "012345") == 0 && out.token[0] == '\0');
    assert(gateway("{\"websocket\":{\"url\":\"ws://xrobo-io.qiniuapi.com/v1/ws/\",\"token\":\"test.token-123\"}}", &out) == LINX_GATEWAY_READY);
    assert(strcmp(out.url, "wss://xrobo-io.qiniuapi.com/v1/ws/") == 0);
    assert(strcmp(out.token, "test.token-123") == 0 && out.activation_code[0] == '\0');
    assert(gateway("{\"websocket\":{\"url\":\"wss://other.example/ws\",\"token\":\"Bearer test.token-123\"}}", &out) == LINX_GATEWAY_READY);
    assert(strcmp(out.token, "test.token-123") == 0);
    assert(gateway("{\"websocket\":{\"url\":\"wss://other.example/ws\",\"token\":\"t\"}}", &out) == LINX_GATEWAY_READY);
    const char *const invalid[] = {"{}", "{\"error\":null,\"activation\":{\"code\":\"012345\"}}",
        "{\"activation\":{\"code\":123456}}", "{\"activation\":{\"code\":\"12345\"}}",
        "{\"activation\":{\"code\":\"12345x\"}}", "{\"activation\":{\"code\":\"1234567\"}}",
        "{\"activation\":{\"code\":\"123456\"},\"websocket\":{\"token\":123}}",
        "{\"websocket\":{\"token\":\"\"}}", "{\"websocket\":{\"token\":\"a b\"}}",
        "{\"websocket\":{\"token\":\"Bearer \"}}",
        "{\"websocket\":{\"token\":\"a\\r\\nHeader: bad\"}}", "{\"websocket\":{\"token\":\"非ASCII\"}}",
        "{\"websocket\":{\"token\":\"t\"}}", "{\"websocket\":{\"token\":\"t\",\"url\":\"ws://other/ws\"}}",
        "{\"websocket\":{\"token\":\"t\",\"url\":\"wss://host/#fragment\"}}"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) assert(gateway(invalid[i], &out) == LINX_GATEWAY_INVALID);
    char json[1000];
    memset(json, 'a', sizeof(json));
    strcpy(json, "{\"websocket\":{\"token\":\"");
    size_t prefix = strlen(json);
    memset(json + prefix, 't', LINX_TOKEN_CAPACITY);
    strcpy(json + prefix + LINX_TOKEN_CAPACITY, "\",\"url\":\"wss://host/\"}}");
    assert(gateway(json, &out) == LINX_GATEWAY_INVALID);
    assert(linx_parse_gateway("{}", 2, NULL) == LINX_GATEWAY_INVALID);
}

static bool message(const char *json, linx_message_t *out)
{
    return linx_parse_message(json, strlen(json), out);
}

static bool audio_message(const char *audio, linx_message_t *out)
{
    char json[1000];
    snprintf(json, sizeof(json), "{\"type\":\"hello\",\"transport\":\"websocket\",\"session_id\":\"s1\",\"audio_params\":%s}", audio);
    return message(json, out);
}

static void test_messages(void)
{
    linx_message_t out;
    assert(audio_message("{\"format\":\"opus\",\"channels\":1,\"sample_rate\":24000,\"frame_duration\":60}", &out));
    assert(out.kind == LINX_HELLO && out.audio == LINX_AUDIO_OPUS && out.sample_rate == 24000);
    assert(strcmp(out.session_id, "s1") == 0);
    assert(audio_message("{\"format\":\"opus\",\"channels\":1,\"sample_rate\":16000}", &out));
    assert(out.sample_rate == 16000);
    assert(audio_message("{\"format\":\"pcm\",\"channels\":1,\"sample_rate\":16000,\"frame_duration\":20,\"bit_depth\":16,\"endianness\":\"little\",\"sample_format\":\"signed_int16\"}", &out));
    assert(out.audio == LINX_AUDIO_PCM && out.sample_rate == 16000);
    assert(audio_message("{\"format\":\"pcm\",\"channels\":1,\"sample_rate\":16000}", &out));
    const char *const bad_audio[] = {"null", "[]", "{\"format\":8}", "{\"sample_rate\":\"16000\"}",
        "{\"format\":\"opus\",\"channels\":2,\"sample_rate\":16000}",
        "{\"format\":\"opus\",\"channels\":\"1\",\"sample_rate\":16000}",
        "{\"format\":\"mp3\",\"channels\":1,\"sample_rate\":16000}",
        "{\"format\":null}", "{\"format\":\"opus\",\"channels\":1,\"sample_rate\":48000}",
        "{\"format\":\"opus\",\"channels\":1,\"sample_rate\":16000.1}",
        "{\"format\":\"pcm\",\"channels\":1,\"sample_rate\":24000}",
        "{\"format\":\"pcm\",\"channels\":1,\"sample_rate\":16000,\"bit_depth\":8}",
        "{\"format\":\"pcm\",\"channels\":1,\"sample_rate\":16000,\"endianness\":\"big\"}",
        "{\"format\":\"pcm\",\"channels\":1,\"sample_rate\":16000,\"endianness\":2}",
        "{\"format\":\"pcm\",\"channels\":1,\"sample_rate\":16000,\"sample_format\":\"float\"}",
        "{\"format\":\"pcm\",\"channels\":1,\"sample_rate\":16000,\"sample_format\":false}",
        "{\"format\":\"opus\",\"channels\":1,\"sample_rate\":16000,\"frame_duration\":\"20\"}",
        "{\"format\":\"opus\",\"channels\":1,\"sample_rate\":16000,\"frame_duration\":0}",
        "{\"format\":\"opus\",\"channels\":1,\"sample_rate\":16000,\"frame_duration\":121}"};
    for (size_t i = 0; i < sizeof(bad_audio) / sizeof(bad_audio[0]); ++i) assert(!audio_message(bad_audio[i], &out));
    /* 小智固定版本的最小 hello、可选音频字段和默认24k Opus。 */
    const char *const minimal_audio[] = {"{}", "{\"format\":\"opus\"}",
        "{\"format\":\"opus\",\"channels\":1}", "{\"sample_rate\":24000}"};
    for (size_t i = 0; i < sizeof(minimal_audio) / sizeof(minimal_audio[0]); ++i) {
        assert(audio_message(minimal_audio[i], &out));
        assert(out.kind == LINX_HELLO && out.audio == LINX_AUDIO_OPUS && out.sample_rate == 24000);
    }
    assert(message("{\"type\":\"hello\",\"transport\":\"websocket\"}", &out));
    assert(out.audio == LINX_AUDIO_OPUS && out.sample_rate == 24000 && out.session_id[0] == '\0');
    assert(audio_message("{\"format\":\"opus\",\"sample_rate\":16000}", &out));
    assert(out.audio == LINX_AUDIO_OPUS && out.sample_rate == 16000);
    assert(audio_message("{\"format\":\"pcm\"}", &out));
    assert(out.audio == LINX_AUDIO_PCM && out.sample_rate == 16000);
    assert(message("{\"type\":\"stt\",\"session_id\":\"old-session\",\"text\":\"不显示动态文字\"}", &out));
    assert(out.kind == LINX_STT && strcmp(out.session_id, "old-session") == 0);
    assert(message("{\"type\":\"tts\",\"state\":\"start\"}", &out));
    assert(out.kind == LINX_TTS_START && !out.aborted && out.session_id[0] == '\0');
    assert(message("{\"type\":\"tts\",\"state\":\"stop\",\"is_aborted\":true}", &out));
    assert(out.kind == LINX_TTS_STOP && out.aborted);
    assert(message("{\"type\":\"tts\",\"state\":\"sentence_start\",\"is_aborted\":false}", &out));
    assert(out.kind == LINX_IGNORE && !out.aborted);
    assert(message("{\"type\":\"llm\",\"text\":\"{}[]\\\"\\\\\"}", &out));
    assert(out.kind == LINX_IGNORE);
    assert(message("{\"type\":\"notify\",\"session_id\":\"\"} \n\r\t", &out));
    assert(out.kind == LINX_IGNORE);
    const char *const invalid[] = {"{}", "{\"type\":7}", "{\"type\":\"stt\",\"error\":\"no\"}",
        "{\"type\":\"stt\",\"session_id\":5}", "{\"type\":\"stt\",\"session_id\":\"bad\\n\"}",
        "{\"type\":\"hello\"}", "{\"type\":\"hello\",\"transport\":\"mqtt\"}",
        "{\"type\":\"tts\"}", "{\"type\":\"tts\",\"state\":\"stop\",\"is_aborted\":1}"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) assert(!message(invalid[i], &out));
    char long_session[250];
    strcpy(long_session, "{\"type\":\"stt\",\"session_id\":\"");
    size_t prefix = strlen(long_session);
    memset(long_session + prefix, 's', LINX_SESSION_CAPACITY);
    strcpy(long_session + prefix + LINX_SESSION_CAPACITY, "\"}");
    assert(!message(long_session, &out));
    assert(!linx_parse_message("{}", 2, NULL));
}

static void test_bounded_json(void)
{
    linx_message_t out;
    linx_gateway_t gw;
    const char *const invalid[] = {"", "[{}]", "null", "true", "{}x", "{} {}", "{bad}",
        "}", "{", "{\"type\":\"stt}", "{\"type\":\"stt\",\"text\":\"\\", "{\"type\":\"stt\\u0000hidden\"}"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        assert(!message(invalid[i], &out));
        assert(gateway(invalid[i], &gw) == LINX_GATEWAY_INVALID);
    }
    assert(!linx_parse_message(NULL, 1, &out));
    const char nul_json[] = {'{', '}', '\0', 'x'};
    assert(!linx_parse_message(nul_json, sizeof(nul_json), &out));
    assert(!message("{\"type\":\"llm\",\"text\":\"raw\nnewline\"}", &out));
    const char *valid = "{\"type\":\"stt\"}";
    for (size_t size = 1; size < strlen(valid); ++size) assert(!linx_parse_message(valid, size, &out));
    char oversized[LINX_MESSAGE_CAPACITY + 1U];
    memset(oversized, ' ', sizeof(oversized));
    memcpy(oversized, valid, strlen(valid));
    assert(linx_parse_message(oversized, LINX_MESSAGE_CAPACITY, &out));
    assert(!linx_parse_message(oversized, sizeof(oversized), &out));
    char nested[200];
    for (unsigned depth = 15; depth <= 16; ++depth) {
        strcpy(nested, "{\"type\":\"stt\",\"a\":");
        size_t offset = strlen(nested);
        memset(nested + offset, '[', depth);
        offset += depth;
        nested[offset++] = '0';
        memset(nested + offset, ']', depth);
        offset += depth;
        strcpy(nested + offset, "}");
        assert(message(nested, &out) == (depth == 15));
    }
}

static void test_receiver(void)
{
    linx_receiver_t receiver;
    linx_receiver_init(NULL);
    linx_receiver_init(&receiver);
    bool complete = true;
    assert(linx_receive(&receiver, 1, true, 5, "he", 2, &complete) && !complete);
    assert(receiver.used == 2 && receiver.remaining == 3 && receiver.frame_active);
    assert(!linx_receive(&receiver, 1, false, 5, "llo", 3, &complete));
    assert(!linx_receive(&receiver, 1, true, 4, "llo", 3, &complete));
    assert(!linx_receive(&receiver, 0, true, 5, "llo", 3, &complete));
    assert(!linx_receive(&receiver, 9, true, 0, NULL, 0, &complete));
    assert(!linx_receive(&receiver, 1, true, 5, "llll", 4, &complete));
    assert(linx_receive(&receiver, 1, true, 5, "llo", 3, &complete) && complete);
    assert(receiver.kind == 1 && receiver.used == 5 && strcmp((char *)receiver.data, "hello") == 0);
    assert(!linx_receive(&receiver, 0, true, 1, "x", 1, &complete));
    assert(linx_receive(&receiver, 2, false, 2, "a", 1, &complete) && !complete);
    assert(linx_receive(&receiver, 2, false, 2, "b", 1, &complete) && !complete);
    assert(!linx_receive(&receiver, 1, true, 1, "x", 1, &complete));
    assert(linx_receive(&receiver, 9, true, 3, "p", 1, &complete) && !complete);
    assert(!linx_receive(&receiver, 10, true, 3, "ng", 2, &complete));
    assert(!linx_receive(&receiver, 9, true, 2, "ng", 2, &complete));
    assert(!linx_receive(&receiver, 0, true, 2, "cd", 2, &complete));
    assert(!linx_receive(&receiver, 9, true, 3, "ong", 3, &complete));
    assert(linx_receive(&receiver, 9, true, 3, "ng", 2, &complete) && !complete);
    assert(receiver.kind == 2 && receiver.used == 2 && memcmp(receiver.data, "ab", 2) == 0);
    assert(linx_receive(&receiver, 10, true, 0, NULL, 0, &complete) && !complete);
    assert(linx_receive(&receiver, 8, true, 0, NULL, 0, &complete) && !complete);
    assert(linx_receive(&receiver, 0, false, 1, "c", 1, &complete) && !complete);
    assert(linx_receive(&receiver, 0, true, 2, "d", 1, &complete) && !complete);
    assert(linx_receive(&receiver, 0, true, 2, "e", 1, &complete) && complete);
    assert(receiver.kind == 2 && receiver.used == 5 && memcmp(receiver.data, "abcde", 5) == 0);
    assert(receiver.data[5] == 0);
    assert(linx_receive(&receiver, 1, true, 0, NULL, 0, &complete) && complete && receiver.used == 0);
    linx_receiver_init(&receiver);
    assert(!linx_receive(&receiver, 0, true, 0, NULL, 0, &complete));
    assert(!linx_receive(&receiver, 3, true, 0, NULL, 0, &complete));
    assert(!linx_receive(&receiver, 9, false, 1, "a", 1, &complete));
    assert(!linx_receive(&receiver, 9, true, 126, "a", 1, &complete));
    assert(!linx_receive(NULL, 1, true, 0, NULL, 0, &complete));
    assert(!linx_receive(&receiver, 1, true, 0, NULL, 0, NULL));
    assert(!linx_receive(&receiver, 1, true, 1, NULL, 1, &complete));
    char full[LINX_MESSAGE_CAPACITY];
    memset(full, 'x', sizeof(full));
    assert(linx_receive(&receiver, 2, false, sizeof(full), full, sizeof(full), &complete) && !complete);
    assert(!linx_receive(&receiver, 0, true, 1, "x", 1, &complete));
    assert(linx_receive(&receiver, 0, true, 0, NULL, 0, &complete) && complete);
    assert(receiver.used == sizeof(full) && receiver.data[sizeof(full)] == 0);
    assert(!linx_receive(&receiver, 2, true, SIZE_MAX, full, 1, &complete));
    linx_receiver_init(&receiver);
    receiver.used = LINX_MESSAGE_CAPACITY + 1U;
    receiver.kind = 2;
    assert(!linx_receive(&receiver, 0, true, 0, NULL, 0, &complete));
}

static size_t allocations_left;
static size_t live_allocations;

static void *failing_malloc(size_t size)
{
    if (allocations_left == 0) return NULL;
    --allocations_left;
    void *allocation = malloc(size);
    if (allocation != NULL) ++live_allocations;
    return allocation;
}

static void counted_free(void *allocation)
{
    if (allocation != NULL) {
        assert(live_allocations > 0);
        --live_allocations;
        free(allocation);
    }
}

static void test_allocation_failures(void)
{
    cJSON_Hooks hooks = {.malloc_fn = failing_malloc, .free_fn = counted_free};
    for (unsigned operation = 0; operation < 6; ++operation) {
        bool success = false;
        for (size_t allowed = 0; allowed < 150 && !success; ++allowed) {
            char output[4096];
            allocations_left = allowed;
            cJSON_InitHooks(&hooks);
            switch (operation) {
                case 0: success = linx_build_ota(output, sizeof(output), &DEVICE); break;
                case 1: success = linx_build_hello(output, sizeof(output), &CONTEXT); break;
                case 2: success = linx_build_listen(output, sizeof(output), "session", true); break;
                case 3: {
                    linx_gateway_t out;
                    success = gateway("{\"websocket\":{\"token\":\"t\",\"url\":\"wss://host/\"}}", &out) == LINX_GATEWAY_READY;
                    break;
                }
                case 5: success = linx_build_listen_request(output, sizeof(output), "session", "request"); break;
                default: {
                    linx_message_t out;
                    success = message("{\"type\":\"stt\"}", &out);
                    break;
                }
            }
            assert(live_allocations == 0);
            cJSON_InitHooks(NULL);
            if (!success && (operation < 3 || operation == 5)) assert(output[0] == '\0');
        }
        assert(success);
    }
}

static void test_listen_request_id(void)
{
    char output[2048], original[2048], id[514];
    assert(!linx_protocol_id_valid(NULL) && !linx_protocol_id_valid(""));
    assert(linx_build_listen(original, sizeof(original), "session", true));
    assert(linx_build_listen_request(output, sizeof(output), "session", NULL));
    assert(!strcmp(output, original));
    assert(linx_build_listen_request(output, sizeof(output), "session", ""));
    assert(!strcmp(output, original));
    assert(linx_build_listen_request(output, sizeof(output), "session", "轮次\"\\id"));
    cJSON *root = cJSON_Parse(output);
    expect_string(root, "request_id", "轮次\"\\id");
    expect_string(root, "state", "start"); cJSON_Delete(root);
    for (size_t i = 0; i < 128; ++i) memcpy(id + 4 * i, "\xf0\x9f\x98\x80", 4);
    id[512] = 0;
    assert(linx_build_listen_request(output, sizeof(output), "session", id));
    assert(!linx_build_listen_request(NULL, sizeof(output), "session", id));
    assert(!linx_build_listen_request(output, 1, "session", id));
    id[512] = 'x'; id[513] = 0;
    assert(!linx_build_listen_request(output, sizeof(output), "session", id));
    memset(id, 'a', 129); id[129] = 0;
    assert(!linx_build_listen_request(output, sizeof(output), "session", id));
    const char *bad[] = {"\x80", "\xc0\x80", "\xc2", "\xc2x", "\xe0\x80\x80", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xf5\x80\x80\x80", "bad\n", "\x7f"};
    for (size_t i = 0; i < sizeof(bad)/sizeof(bad[0]); ++i)
        assert(!linx_build_listen_request(output, sizeof(output), "session", bad[i]));
    assert(linx_build_listen_request(output, sizeof(output), "session", "\xc2\xa2\xe4\xb8\xad"));
    assert(linx_build_listen(output, sizeof(output), "session", false) && !strstr(output, "request_id"));
    assert(linx_build_abort(output, sizeof(output), "session") && !strstr(output, "request_id"));
}

int main(void)
{
    test_listen_request_id();
    test_builders();
    test_endpoints();
    test_gateway();
    test_messages();
    test_bounded_json();
    test_receiver();
    test_allocation_failures();
    puts("灵矽协议测试通过：OTA、握手、控制、音频协商、有界分块与分片、异常和分配失败");
    return 0;
}
