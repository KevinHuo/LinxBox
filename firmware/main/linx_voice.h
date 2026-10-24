#ifndef LINX_VOICE_H
#define LINX_VOICE_H

#include <stdbool.h>
#include <stdint.h>
#include "linx_demo.h"
#include "linx_wifi.h"

typedef enum {
    LINX_VOICE_START,
    LINX_VOICE_FINISH,
    LINX_VOICE_CANCEL,
    LINX_VOICE_WIFI_SCAN,
    LINX_VOICE_WIFI_SAVE,
    LINX_VOICE_WIFI_SETUP
} linx_voice_command_t;

typedef enum {
    LINX_VOICE_CONNECTING,
    LINX_VOICE_BINDING,
    LINX_VOICE_LISTENING,
    LINX_VOICE_WAITING,
    LINX_VOICE_SPEAKING,
    LINX_VOICE_ERROR,
    LINX_VOICE_CLOSED,
    LINX_VOICE_WIFI_SCANNING,
    LINX_VOICE_WIFI_NETWORKS,
    LINX_VOICE_WIFI_CONNECTING,
    LINX_VOICE_WIFI_SAVED,
    LINX_VOICE_WIFI_PORTAL
} linx_voice_state_t;

typedef enum {
    LINX_VOICE_OK,
    LINX_VOICE_NOT_CONFIGURED,
    LINX_VOICE_WIFI_FAILED,
    LINX_VOICE_TIME_FAILED,
    LINX_VOICE_NETWORK_FAILED,
    LINX_VOICE_PROTOCOL_FAILED,
    LINX_VOICE_AUDIO_FAILED,
    LINX_VOICE_NO_MEMORY,
    LINX_VOICE_TIMEOUT
} linx_voice_error_t;

typedef struct {
    linx_voice_command_t command;
    uint32_t generation;
    linx_reply_context_t context;
    linx_wifi_network_t wifi_network;
    char wifi_password[LINX_WIFI_PASSWORD_CAPACITY];
} linx_voice_request_t;

typedef struct {
    uint32_t generation;
    linx_voice_state_t state;
    linx_voice_error_t error;
    char activation_code[7];
    size_t wifi_count;
    linx_wifi_network_t wifi_networks[LINX_WIFI_NETWORK_CAPACITY];
    char wifi_ap_ssid[LINX_WIFI_SSID_CAPACITY];
    char wifi_ap_password[9];
} linx_voice_event_t;

/* 只投递命令，不得在调用者持有 LVGL 锁时阻塞或访问音频、网络。 */
typedef bool (*linx_voice_handler_t)(const linx_voice_request_t *request, void *user);

#endif
