#ifndef LINX_INPUT_SETTINGS_H
#define LINX_INPUT_SETTINGS_H

#include <stdbool.h>
#include "linx_input.h"

typedef struct {
    linx_back_mode_t mode;
    bool saved;
} linx_input_settings_event_t;

/* 在 LVGL 锁外初始化；没有保存值时使用长按，不擦除已有 NVS。 */
bool linx_input_settings_init(linx_back_mode_t *loaded_mode);
/* 只投递保存请求，NVS 写入由独立任务执行。 */
bool linx_input_settings_request(linx_back_mode_t mode, void *user);
bool linx_input_settings_poll(linx_input_settings_event_t *event);

#endif
