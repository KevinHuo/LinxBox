#ifndef LINX_DEVICE_H
#define LINX_DEVICE_H

#include <stdbool.h>
#include "linx_voice.h"

/* 初始化仅创建消息队列和工作任务；无线及音频在 START 命令后才启动。 */
bool linx_device_init(void);
bool linx_device_request(const linx_voice_request_t *request, void *user);
bool linx_device_poll(linx_voice_event_t *event);

#endif
