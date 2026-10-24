#ifndef LINX_WIFI_PORTAL_H
#define LINX_WIFI_PORTAL_H

#include "esp_err.h"
#include "linx_wifi.h"

typedef struct linx_wifi_portal linx_wifi_portal_t;
typedef enum {
    LINX_WIFI_PORTAL_READY,
    LINX_WIFI_PORTAL_CONNECTING,
    LINX_WIFI_PORTAL_SAVED,
    LINX_WIFI_PORTAL_ERROR
} linx_wifi_portal_state_t;

/* 仅管理临时网页及有界请求；无线连接、NVS 和取消由设备工作任务负责。 */
esp_err_t linx_wifi_portal_start(linx_wifi_portal_t **portal,
                               const linx_wifi_network_t *networks, size_t count);
bool linx_wifi_portal_poll(linx_wifi_portal_t *portal, linx_wifi_network_t *network,
                          char password[LINX_WIFI_PASSWORD_CAPACITY]);
void linx_wifi_portal_status(linx_wifi_portal_t *portal, linx_wifi_portal_state_t state);
/* 停止失败时保留实例，调用方须重试，成功后实例失效。 */
esp_err_t linx_wifi_portal_stop(linx_wifi_portal_t *portal);

#endif
