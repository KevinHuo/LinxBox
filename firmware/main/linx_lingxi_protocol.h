#ifndef LINX_LINGXI_PROTOCOL_H
#define LINX_LINGXI_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "linx_demo.h"

#define LINX_LINGXI_MESSAGE_CAPACITY 4096U
#define LINX_LINGXI_SESSION_CAPACITY 96U
#define LINX_LINGXI_TOKEN_CAPACITY 512U
#define LINX_LINGXI_URL_CAPACITY 256U
#define LINX_LINGXI_PCM_FRAME_BYTES 640U
#define LINX_LINGXI_PCM_OUTPUT_BYTES 3840U

typedef struct {
    const char *device_id;
    const char *client_id;
    const char *version;
    const char *elf_sha256;
    const char *ssid;
    int rssi;
} linx_lingxi_device_info_t;

typedef struct {
    char url[LINX_LINGXI_URL_CAPACITY];
    char token[LINX_LINGXI_TOKEN_CAPACITY];
    char activation_code[7];
} linx_lingxi_gateway_t;

typedef enum {
    LINX_LINGXI_GATEWAY_INVALID,
    LINX_LINGXI_GATEWAY_BINDING,
    LINX_LINGXI_GATEWAY_READY
} linx_lingxi_gateway_result_t;

typedef enum {
    LINX_LINGXI_AUDIO_OPUS,
    LINX_LINGXI_AUDIO_PCM
} linx_lingxi_audio_t;

typedef enum {
    LINX_LINGXI_HELLO,
    LINX_LINGXI_STT,
    LINX_LINGXI_TTS_START,
    LINX_LINGXI_TTS_STOP,
    LINX_LINGXI_IGNORE
} linx_lingxi_message_kind_t;

typedef struct {
    linx_lingxi_message_kind_t kind;
    char session_id[LINX_LINGXI_SESSION_CAPACITY];
    linx_lingxi_audio_t audio;
    uint32_t sample_rate;
    bool aborted;
} linx_lingxi_message_t;

typedef struct {
    char host[128];
    char path[LINX_LINGXI_URL_CAPACITY];
    int port;
} linx_lingxi_endpoint_t;

/* 所有消息受同一容量上限约束；一个 WebSocket 包允许多次读取和 continuation。 */
typedef struct {
    uint8_t data[LINX_LINGXI_MESSAGE_CAPACITY + 1U];
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
} linx_lingxi_receiver_t;

bool linx_lingxi_build_ota(char *out, size_t capacity, const linx_lingxi_device_info_t *device);
linx_lingxi_gateway_result_t linx_lingxi_parse_gateway(const char *json, size_t size,
                                                     linx_lingxi_gateway_t *out);
bool linx_lingxi_parse_endpoint(const char *url, linx_lingxi_endpoint_t *out);
bool linx_lingxi_build_hello(char *out, size_t capacity, const linx_reply_context_t *context);
bool linx_lingxi_build_listen(char *out, size_t capacity, const char *session_id, bool start);
bool linx_lingxi_build_abort(char *out, size_t capacity, const char *session_id);
bool linx_lingxi_parse_message(const char *json, size_t size, linx_lingxi_message_t *out);
void linx_lingxi_receiver_init(linx_lingxi_receiver_t *receiver);
bool linx_lingxi_receive(linx_lingxi_receiver_t *receiver, unsigned opcode, bool fin,
                        size_t payload_size, const void *chunk, size_t size, bool *complete);

#endif
