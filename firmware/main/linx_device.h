#ifndef LINX_DEVICE_H
#define LINX_DEVICE_H

#include <stdbool.h>
#include <stdint.h>
#include "linx_voice.h"

/* 初始化只创建队列和工作任务；PREPARE 可联网，只有 START 才开启录音。 */
bool linx_device_init(void);
bool linx_device_request(const linx_voice_request_t *request, void *user);
bool linx_device_poll(linx_voice_event_t *event);
typedef struct {
    bool ready;
    uint32_t epoch;
    bool provisioning;
    linx_voice_error_t error;
    char mac_address[18];
} linx_device_network_state_t;

/* 常驻网络意图独立于语音命令槽及语音 generation。 */
bool linx_device_network_acquire(void);
void linx_device_network_release(void);
bool linx_device_network_poll(linx_device_network_state_t *state);
/* 传输已关闭旧 epoch 后确认；配网必须等待确认才拆除 STA。 */
void linx_device_network_closed(uint32_t epoch);

#endif
