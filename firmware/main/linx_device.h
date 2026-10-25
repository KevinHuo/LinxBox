#ifndef LINX_DEVICE_H
#define LINX_DEVICE_H

#include <stdbool.h>
#include "linx_voice.h"

/* 初始化只创建队列和工作任务；PREPARE 可联网，只有 START 才开启录音。 */
bool linx_device_init(void);
bool linx_device_request(const linx_voice_request_t *request, void *user);
bool linx_device_poll(linx_voice_event_t *event);

#endif
