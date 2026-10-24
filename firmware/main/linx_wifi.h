#ifndef LINX_WIFI_H
#define LINX_WIFI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LINX_WIFI_SSID_CAPACITY 33U
#define LINX_WIFI_PASSWORD_CAPACITY 65U
#define LINX_WIFI_NETWORK_CAPACITY 8U

typedef enum { LINX_WIFI_OPEN, LINX_WIFI_PERSONAL, LINX_WIFI_UNSUPPORTED } linx_wifi_security_t;
typedef struct {
    char ssid[LINX_WIFI_SSID_CAPACITY];
    int8_t rssi;
    linx_wifi_security_t security;
} linx_wifi_network_t;

/* 个人网络密码为 8–63 位可打印 ASCII 或 64 位十六进制；开放网络为空。 */
bool linx_wifi_password_valid(const char *password, bool secure);

#endif
