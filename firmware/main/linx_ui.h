#ifndef LINX_UI_H
#define LINX_UI_H

#include <stddef.h>
#include <stdint.h>
#include "lvgl.h"
#include "linx_demo.h"
#include "linx_voice.h"

typedef enum {
    LINX_UI_KEY_UP,
    LINX_UI_KEY_DOWN,
    LINX_UI_KEY_OK,
    LINX_UI_KEY_OK_LONG
} linx_ui_key_t;

typedef enum {
    LINX_UI_BOOT,
    LINX_UI_TASKS,
    LINX_UI_OVERVIEW,
    LINX_UI_READER,
    LINX_UI_HISTORY,
    LINX_UI_IMAGES,
    LINX_UI_IMAGE_MENU,
    LINX_UI_IMAGE_DETAIL,
    LINX_UI_VOICE,
    LINX_UI_CONFIRM,
    LINX_UI_RECEIPT,
    LINX_UI_SETTINGS,
    LINX_UI_RESET,
    LINX_UI_ABOUT,
    LINX_UI_WIFI
} linx_ui_page_t;

typedef enum {
    LINX_UI_WIFI_SCANNING,
    LINX_UI_WIFI_PHONE,
    LINX_UI_WIFI_CONNECTING,
    LINX_UI_WIFI_CONNECTED,
    LINX_UI_WIFI_ERROR
} linx_ui_wifi_mode_t;

typedef struct {
    linx_ui_page_t page;
    uint32_t task_id;
    uint32_t turn_id;
    uint32_t command_id;
    uint32_t image_id;
    size_t selected;
    size_t read_page;
    size_t read_page_count;
    unsigned image_view;
    bool render_overflow;
    size_t labels_used;
    uint32_t voice_generation;
    linx_voice_state_t voice_state;
    linx_voice_error_t voice_error;
    linx_ui_wifi_mode_t wifi_mode;
} linx_ui_debug_t;

/* Call only from a serialized LVGL context (or while the BSP LVGL lock is held).
 * The caller owns screen; deinit removes only children created by this module.
 * Input must already suppress a click after an OK long press.
 */
void linx_ui_init(lv_obj_t *screen, uint64_t now_ms);
void linx_ui_handle_key(linx_ui_key_t key);
void linx_ui_tick(uint64_t now_ms);
void linx_ui_deinit(void);

/* 在同一 LVGL 上下文内注册非阻塞后端、接收主循环转交的状态快照。 */
void linx_ui_set_voice_handler(linx_voice_handler_t handler, void *user);
bool linx_ui_voice_event(const linx_voice_event_t *event);

/* Read-only diagnostics shared by the desktop render harness and device logs. */
linx_ui_page_t linx_ui_page(void);
const linx_demo_t *linx_ui_demo(void);
linx_ui_debug_t linx_ui_debug(void);

#endif
