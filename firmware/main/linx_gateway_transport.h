#ifndef LINX_GATEWAY_TRANSPORT_H
#define LINX_GATEWAY_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LINX_GATEWAY_FRAME_CAPACITY 65536U
/* 仅有界控制消息：ID/游标最坏UTF-8与JSON转义；禁止发送录音原文。 */
#define LINX_GATEWAY_SEND_CAPACITY 2048U

typedef enum {
    LINX_GATEWAY_TRANSPORT_CONNECTED,
    LINX_GATEWAY_TRANSPORT_DISCONNECTED,
    LINX_GATEWAY_TRANSPORT_FRAME,
    LINX_GATEWAY_TRANSPORT_ERROR,
    LINX_GATEWAY_TRANSPORT_NOT_CONFIGURED
} linx_gateway_transport_event_type_t;

typedef struct {
    linx_gateway_transport_event_type_t type;
    uint32_t generation;
    char mac_address[18];
    const char *data;
    size_t size;
    int error;
} linx_gateway_transport_event_t;

/* 网络和收发均在工作任务中执行，poll/send 不阻塞调用者。 */
bool linx_gateway_transport_init(void);
bool linx_gateway_transport_poll(linx_gateway_transport_event_t *event);
/* FRAME 借出的 data 在 release 之前保持有效；一帧只借出一次。 */
void linx_gateway_transport_release(void);
bool linx_gateway_transport_send(uint32_t generation, const char *json, size_t size, void *user);
void linx_gateway_transport_retry(void);
/* 异步停用；任务先关闭 WSS 并确认网络 epoch，再释放网络意图。 */
void linx_gateway_transport_deinit(void);

#endif
