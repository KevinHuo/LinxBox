#ifndef LINX_PROTOCOL_H
#define LINX_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "linx_demo.h"

#define LINX_MESSAGE_CAPACITY 4096U
#define LINX_SESSION_CAPACITY 96U
#define LINX_TOKEN_CAPACITY 512U
#define LINX_URL_CAPACITY 256U
#define LINX_PCM_FRAME_BYTES 640U
#define LINX_PCM_OUTPUT_BYTES 3840U

typedef struct {
    const char *device_id;
    const char *client_id;
    const char *version;
    const char *elf_sha256;
    const char *ssid;
    int rssi;
} linx_device_info_t;

typedef struct {
    char url[LINX_URL_CAPACITY];
    char token[LINX_TOKEN_CAPACITY];
    char activation_code[7];
} linx_gateway_t;

typedef enum {
    LINX_GATEWAY_INVALID,
    LINX_GATEWAY_BINDING,
    LINX_GATEWAY_READY
} linx_gateway_result_t;

typedef enum {
    LINX_AUDIO_OPUS,
    LINX_AUDIO_PCM
} linx_audio_t;

typedef enum {
    LINX_HELLO,
    LINX_STT,
    LINX_TTS_START,
    LINX_TTS_STOP,
    LINX_IGNORE
} linx_message_kind_t;

typedef struct {
    linx_message_kind_t kind;
    char session_id[LINX_SESSION_CAPACITY];
    linx_audio_t audio;
    uint32_t sample_rate;
    bool aborted;
} linx_message_t;

typedef struct {
    char host[128];
    char path[LINX_URL_CAPACITY];
    int port;
    bool tls;
} linx_endpoint_t;

/* 所有消息受同一容量上限约束；一个 WebSocket 包允许多次读取和 continuation。 */
typedef struct {
    uint8_t data[LINX_MESSAGE_CAPACITY + 1U];
    size_t used;
    size_t remaining;
    unsigned kind;
    unsigned frame_opcode;
    size_t frame_size;
    size_t control_remaining;
    size_t control_size;
    unsigned control_opcode;
    bool frame_active;
    bool frame_fin;
} linx_receiver_t;

bool linx_build_ota(char *out, size_t capacity, const linx_device_info_t *device);
linx_gateway_result_t linx_parse_gateway(const char *json, size_t size,
                                                     linx_gateway_t *out);
bool linx_parse_endpoint(const char *url, linx_endpoint_t *out);
/* 仅 Gateway 可显式允许明文 WS；灵矽继续使用严格 WSS 入口。 */
bool linx_parse_gateway_endpoint(const char *url, bool allow_ws, linx_endpoint_t *out);
bool linx_build_hello(char *out, size_t capacity, const linx_reply_context_t *context);
bool linx_build_listen(char *out, size_t capacity, const char *session_id, bool start);
/* 仅传递本轮编号元数据，可信 MCP 透传能力须单独验收。 */
bool linx_build_listen_request(char *out, size_t capacity, const char *session_id,
                               const char *request_id);
bool linx_protocol_id_valid(const char *id);
bool linx_build_abort(char *out, size_t capacity, const char *session_id);
bool linx_parse_message(const char *json, size_t size, linx_message_t *out);
void linx_receiver_init(linx_receiver_t *receiver);
bool linx_receive(linx_receiver_t *receiver, unsigned opcode, bool fin,
                        size_t payload_size, const void *chunk, size_t size, bool *complete);

#endif
