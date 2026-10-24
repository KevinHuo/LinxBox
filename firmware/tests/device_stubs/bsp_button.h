#pragma once

#include "esp_err.h"

typedef enum { BSP_BTN_UP, BSP_BTN_DOWN, BSP_BTN_OK } bsp_btn_t;
typedef enum {
    BSP_BTN_PRESS,
    BSP_BTN_CLICK,
    BSP_BTN_DOUBLE,
    BSP_BTN_LONG,
    BSP_BTN_RELEASE
} bsp_btn_ev_t;
typedef void (*bsp_btn_cb_t)(bsp_btn_t key, bsp_btn_ev_t event, void *user);

esp_err_t bsp_button_init(bsp_btn_cb_t callback, void *user);
int bsp_button_read_mv(void);
