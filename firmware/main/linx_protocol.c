/* SPDX-License-Identifier: MIT
 * Copyright (c) 2025 Shenzhen Xinzhi Future Technology Co., Ltd.
 * Copyright (c) 2025 Project Contributors
 * 小智 hello/listen/abort 与 hello 默认参数移植，来源及适配说明见 upstream.json。
 * https://github.com/78/xiaozhi-esp32/tree/39d23aee5b8682f77bbdf9cdc66799456b5ad408/main/protocols
 */
#include "linx_protocol.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"

#define JSON_DEPTH_LIMIT 16U

static bool bounded_string(const char *text, size_t capacity, bool empty_allowed)
{
    if (text == NULL) return false;
    size_t size = strlen(text);
    if (size >= capacity || (!empty_allowed && size == 0U)) return false;
    for (size_t i = 0; i < size; ++i) {
        if ((unsigned char)text[i] < 0x20U || (unsigned char)text[i] == 0x7fU) return false;
    }
    return true;
}

/* 在交给 cJSON 前限制递归深度；禁止 NUL，避免其 C 字符串接口截断鉴权字段。 */
static cJSON *parse_object(const char *json, size_t size)
{
    if (json == NULL || size == 0U || size > LINX_MESSAGE_CAPACITY) return NULL;
    unsigned depth = 0;
    bool quoted = false;
    for (size_t i = 0; i < size; ++i) {
        unsigned char byte = (unsigned char)json[i];
        if (byte == 0U) return NULL;
        if (quoted && byte < 0x20U) return NULL;
        if (quoted && byte == '\\') {
            if (++i == size) return NULL;
            if (json[i] == 'u' && size - i >= 5U && memcmp(json + i, "u0000", 5) == 0) return NULL;
        } else if (byte == '"') {
            quoted = !quoted;
        } else if (!quoted && (byte == '{' || byte == '[')) {
            if (++depth > JSON_DEPTH_LIMIT) return NULL;
        } else if (!quoted && (byte == '}' || byte == ']')) {
            if (depth == 0U) return NULL;
            --depth;
        }
    }
    if (quoted || depth != 0U) return NULL;
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(json, size, &end, false);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return NULL;
    }
    while (end < json + size && (*end == ' ' || *end == '\r' || *end == '\n' || *end == '\t')) ++end;
    if (end != json + size) {
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

static const char *string_field(const cJSON *object, const char *name)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(value) ? value->valuestring : NULL;
}

static bool output_ready(char *out, size_t capacity)
{
    if (out == NULL || capacity == 0U) return false;
    out[0] = '\0';
    return true;
}

static bool finish_json(char *out, size_t capacity, cJSON *root, bool valid)
{
    bool result = valid && cJSON_PrintPreallocated(root, out,
        capacity > INT_MAX ? INT_MAX : (int)capacity, false);
    cJSON_Delete(root);
    if (!result) out[0] = '\0';
    return result;
}

bool linx_build_ota(char *out, size_t capacity, const linx_device_info_t *device)
{
    if (!output_ready(out, capacity)) return false;
    if (device == NULL || !bounded_string(device->device_id, 64U, false) ||
        !bounded_string(device->client_id, 64U, false) ||
        !bounded_string(device->version, 96U, false) ||
        !bounded_string(device->elf_sha256, 129U, false) ||
        !bounded_string(device->ssid, 33U, true)) return false;
    cJSON *root = cJSON_CreateObject();
    cJSON *application = cJSON_AddObjectToObject(root, "application");
    cJSON *board = cJSON_AddObjectToObject(root, "board");
    bool valid = root != NULL;
    valid &= cJSON_AddStringToObject(root, "mac_address", device->device_id) != NULL;
    valid &= cJSON_AddStringToObject(root, "uuid", device->client_id) != NULL;
    valid &= cJSON_AddStringToObject(application, "version", device->version) != NULL;
    valid &= cJSON_AddStringToObject(application, "elf_sha256", device->elf_sha256) != NULL;
    valid &= cJSON_AddStringToObject(board, "type", "folotoy-ai-passport") != NULL;
    valid &= cJSON_AddStringToObject(board, "ssid", device->ssid) != NULL;
    valid &= cJSON_AddNumberToObject(board, "rssi", device->rssi) != NULL;
    valid &= application != NULL;
    valid &= board != NULL;
    return finish_json(out, capacity, root, valid);
}

bool linx_parse_endpoint(const char *url, linx_endpoint_t *out)
{
    if (out == NULL) return false;
    memset(out, 0, sizeof(*out));
    if (url == NULL || !bounded_string(url, LINX_URL_CAPACITY, false) || strncmp(url, "wss://", 6) != 0) return false;
    for (const char *p = url; *p != '\0'; ++p) {
        if (*p == '@' || *p == '#' || *p == ' ' || (unsigned char)*p >= 0x7fU) return false;
    }
    const char *host = url + 6;
    const char *end = host + strcspn(host, "/?:");
    size_t host_size = (size_t)(end - host);
    if (host_size == 0U || host_size >= sizeof(out->host)) return false;
    for (size_t i = 0; i < host_size; ++i) {
        char c = host[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '.')) return false;
    }
    int port = 443;
    if (*end == ':') {
        port = 0;
        const char *digits = ++end;
        while (*end >= '0' && *end <= '9') {
            port = port * 10 + (*end++ - '0');
            if (port > 65535) return false;
        }
        if (end == digits || port == 0 || (*end != '\0' && *end != '/' && *end != '?')) return false;
    }
    const char *path = *end == '\0' ? "/" : end;
    int written = snprintf(out->path, sizeof(out->path), "%s%s", *path == '?' ? "/" : "", path);
    if (written < 0 || (size_t)written >= sizeof(out->path)) return false;
    memcpy(out->host, host, host_size);
    out->port = port;
    return true;
}

linx_gateway_result_t linx_parse_gateway(const char *json, size_t size,
                                                     linx_gateway_t *out)
{
    if (out == NULL) return LINX_GATEWAY_INVALID;
    memset(out, 0, sizeof(*out));
    cJSON *root = parse_object(json, size);
    if (root == NULL) return LINX_GATEWAY_INVALID;
    linx_gateway_result_t result = LINX_GATEWAY_INVALID;
    if (cJSON_GetObjectItemCaseSensitive(root, "error") != NULL) goto done;
    const cJSON *websocket = cJSON_GetObjectItemCaseSensitive(root, "websocket");
    const cJSON *token_field = cJSON_GetObjectItemCaseSensitive(websocket, "token");
    if (token_field != NULL && !cJSON_IsString(token_field)) goto done;
    const char *token = string_field(websocket, "token");
    if (token == NULL) {
        const cJSON *activation = cJSON_GetObjectItemCaseSensitive(root, "activation");
        const char *code = string_field(activation, "code");
        if (code == NULL || strlen(code) != 6U || strspn(code, "0123456789") != 6U) goto done;
        memcpy(out->activation_code, code, 7);
        result = LINX_GATEWAY_BINDING;
        goto done;
    }
    if (strncmp(token, "Bearer ", 7) == 0) token += 7;
    if (!bounded_string(token, sizeof(out->token), false)) goto done;
    for (const char *p = token; *p != '\0'; ++p) {
        if ((unsigned char)*p <= 0x20U || (unsigned char)*p > 0x7eU) goto done;
    }
    const char *url = string_field(websocket, "url");
    if (url != NULL && strcmp(url, "ws://xrobo-io.qiniuapi.com/v1/ws/") == 0) url = "wss://xrobo-io.qiniuapi.com/v1/ws/";
    linx_endpoint_t endpoint;
    if (!linx_parse_endpoint(url, &endpoint)) goto done;
    strcpy(out->url, url);
    strcpy(out->token, token);
    result = LINX_GATEWAY_READY;
done:
    cJSON_Delete(root);
    return result;
}

bool linx_build_hello(char *out, size_t capacity, const linx_reply_context_t *context)
{
    if (!output_ready(out, capacity)) return false;
    if (context == NULL) return false;
    cJSON *root = cJSON_CreateObject();
    cJSON *audio = cJSON_AddObjectToObject(root, "audio_params");
    cJSON *agent = cJSON_AddObjectToObject(root, "agent_params");
    cJSON *replace = cJSON_AddObjectToObject(agent, "custom_replace_prompt");
    bool valid = root != NULL;
    valid &= audio != NULL;
    valid &= agent != NULL;
    valid &= replace != NULL;
    valid &= cJSON_AddStringToObject(root, "type", "hello") != NULL;
    valid &= cJSON_AddNumberToObject(root, "version", 1) != NULL;
    valid &= cJSON_AddStringToObject(root, "transport", "websocket") != NULL;
    valid &= cJSON_AddStringToObject(audio, "format", "pcm") != NULL;
    valid &= cJSON_AddNumberToObject(audio, "sample_rate", 16000) != NULL;
    valid &= cJSON_AddNumberToObject(audio, "channels", 1) != NULL;
    valid &= cJSON_AddNumberToObject(audio, "bit_depth", 16) != NULL;
    valid &= cJSON_AddStringToObject(audio, "endianness", "little") != NULL;
    valid &= cJSON_AddNumberToObject(audio, "frame_duration", 20) != NULL;
    valid &= cJSON_AddNumberToObject(audio, "frame_size", 320) != NULL;
    valid &= cJSON_AddStringToObject(audio, "sample_format", "signed_int16") != NULL;
    valid &= cJSON_AddNumberToObject(audio, "play_buffer_duration", 120) != NULL;
    const uint32_t ids[] = {context->task_id, context->source_turn_id, context->image_id};
    const char *const names[] = {"linx_task_id", "linx_turn_id", "linx_image_id"};
    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); ++i) {
        char value[11];
        snprintf(value, sizeof(value), "%lu", (unsigned long)ids[i]);
        valid &= cJSON_AddStringToObject(replace, names[i], value) != NULL;
    }
    return finish_json(out, capacity, root, valid);
}

static bool build_control(char *out, size_t capacity, const char *session, const char *state)
{
    if (!output_ready(out, capacity)) return false;
    if (!bounded_string(session, LINX_SESSION_CAPACITY, true)) return false;
    cJSON *root = cJSON_CreateObject();
    bool valid = root != NULL;
    valid &= cJSON_AddStringToObject(root, "type", state == NULL ? "abort" : "listen") != NULL;
    valid &= cJSON_AddStringToObject(root, "session_id", session) != NULL;
    if (state != NULL) {
        valid &= cJSON_AddStringToObject(root, "state", state) != NULL;
        if (strcmp(state, "start") == 0) valid &= cJSON_AddStringToObject(root, "mode", "manual") != NULL;
    }
    return finish_json(out, capacity, root, valid);
}

bool linx_build_listen(char *out, size_t capacity, const char *session_id, bool start)
{
    return build_control(out, capacity, session_id, start ? "start" : "stop");
}

bool linx_build_abort(char *out, size_t capacity, const char *session_id)
{
    return build_control(out, capacity, session_id, NULL);
}

static bool number_is(const cJSON *object, const char *name, double expected)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsNumber(value) && value->valuedouble == expected;
}

static bool compatible_audio(const cJSON *audio, linx_message_t *out)
{
    /* 小智 ParseServerHello 允许缺省音频参数，默认24k Opus、单声道。 */
    out->audio = LINX_AUDIO_OPUS;
    out->sample_rate = 24000;
    if (audio == NULL) return true;
    if (!cJSON_IsObject(audio)) return false;
    const cJSON *format_value = cJSON_GetObjectItemCaseSensitive(audio, "format");
    if (format_value != NULL && !cJSON_IsString(format_value)) return false;
    const char *format = format_value != NULL ? format_value->valuestring : "opus";
    bool is_opus = strcmp(format, "opus") == 0;
    if (!is_opus && strcmp(format, "pcm") != 0) return false;
    const cJSON *channels = cJSON_GetObjectItemCaseSensitive(audio, "channels");
    if (channels != NULL && !number_is(audio, "channels", 1)) return false;
    const cJSON *rate = cJSON_GetObjectItemCaseSensitive(audio, "sample_rate");
    if (rate != NULL && !number_is(audio, "sample_rate", 16000) &&
        !(is_opus && number_is(audio, "sample_rate", 24000))) return false;
    const cJSON *duration = cJSON_GetObjectItemCaseSensitive(audio, "frame_duration");
    if (duration != NULL && (!cJSON_IsNumber(duration) || duration->valuedouble <= 0 || duration->valuedouble > 120)) return false;
    if (!is_opus) {
        const cJSON *depth = cJSON_GetObjectItemCaseSensitive(audio, "bit_depth");
        const cJSON *endian = cJSON_GetObjectItemCaseSensitive(audio, "endianness");
        const cJSON *sample = cJSON_GetObjectItemCaseSensitive(audio, "sample_format");
        if (depth != NULL && !number_is(audio, "bit_depth", 16)) return false;
        if (endian != NULL && (!cJSON_IsString(endian) || strcmp(endian->valuestring, "little") != 0)) return false;
        if (sample != NULL && (!cJSON_IsString(sample) || strcmp(sample->valuestring, "signed_int16") != 0)) return false;
    }
    out->audio = is_opus ? LINX_AUDIO_OPUS : LINX_AUDIO_PCM;
    out->sample_rate = rate != NULL ? (uint32_t)rate->valuedouble : (is_opus ? 24000U : 16000U);
    return true;
}

bool linx_parse_message(const char *json, size_t size, linx_message_t *out)
{
    if (out == NULL) return false;
    memset(out, 0, sizeof(*out));
    out->kind = LINX_IGNORE;
    cJSON *root = parse_object(json, size);
    if (root == NULL) return false;
    bool valid = false;
    const char *type = string_field(root, "type");
    if (type == NULL || cJSON_GetObjectItemCaseSensitive(root, "error") != NULL) goto done;
    const cJSON *session = cJSON_GetObjectItemCaseSensitive(root, "session_id");
    if (session != NULL) {
        if (!cJSON_IsString(session) || !bounded_string(session->valuestring, sizeof(out->session_id), true)) goto done;
        strcpy(out->session_id, session->valuestring);
    }
    if (strcmp(type, "hello") == 0) {
        const char *transport = string_field(root, "transport");
        if (transport == NULL || strcmp(transport, "websocket") != 0 ||
            !compatible_audio(cJSON_GetObjectItemCaseSensitive(root, "audio_params"), out)) goto done;
        out->kind = LINX_HELLO;
    } else if (strcmp(type, "stt") == 0) {
        out->kind = LINX_STT;
    } else if (strcmp(type, "tts") == 0) {
        const char *state = string_field(root, "state");
        if (state == NULL) goto done;
        const cJSON *aborted = cJSON_GetObjectItemCaseSensitive(root, "is_aborted");
        if (aborted != NULL && !cJSON_IsBool(aborted)) goto done;
        out->aborted = cJSON_IsTrue(aborted);
        if (strcmp(state, "start") == 0) out->kind = LINX_TTS_START;
        else if (strcmp(state, "stop") == 0) out->kind = LINX_TTS_STOP;
    }
    valid = true;
done:
    cJSON_Delete(root);
    return valid;
}

void linx_receiver_init(linx_receiver_t *receiver)
{
    if (receiver != NULL) memset(receiver, 0, sizeof(*receiver));
}

bool linx_receive(linx_receiver_t *receiver, unsigned opcode, bool fin,
                        size_t payload_size, const void *chunk, size_t size, bool *complete)
{
    if (complete == NULL) return false;
    *complete = false;
    if (receiver == NULL || (chunk == NULL && size != 0U)) return false;
    if (opcode == 8U || opcode == 9U || opcode == 10U) {
        if (!fin || payload_size > 125U || receiver->frame_active) return false;
        if (receiver->control_remaining != 0U) {
            if (opcode != receiver->control_opcode || payload_size != receiver->control_size) return false;
        } else {
            receiver->control_opcode = opcode;
            receiver->control_size = payload_size;
            receiver->control_remaining = payload_size;
        }
        if (size > receiver->control_remaining) return false;
        receiver->control_remaining -= size;
        return true;
    }
    if (opcode > 2U || receiver->control_remaining != 0U) return false;
    if (receiver->frame_active) {
        if (opcode != receiver->frame_opcode || fin != receiver->frame_fin ||
            payload_size != receiver->frame_size) return false;
    } else {
        if (opcode == 0U) {
            if (receiver->kind == 0U || receiver->frame_fin) return false;
        } else {
            if (receiver->kind != 0U && !receiver->frame_fin) return false;
            receiver->used = 0;
            receiver->kind = opcode;
        }
        if (receiver->used > LINX_MESSAGE_CAPACITY ||
            payload_size > LINX_MESSAGE_CAPACITY - receiver->used) return false;
        receiver->frame_opcode = opcode;
        receiver->frame_size = payload_size;
        receiver->frame_fin = fin;
        receiver->remaining = payload_size;
        receiver->frame_active = true;
    }
    if (size > receiver->remaining) return false;
    if (size != 0U) memcpy(receiver->data + receiver->used, chunk, size);
    receiver->used += size;
    receiver->remaining -= size;
    receiver->data[receiver->used] = '\0';
    if (receiver->remaining == 0U) {
        receiver->frame_active = false;
        *complete = receiver->frame_fin;
    }
    return true;
}
