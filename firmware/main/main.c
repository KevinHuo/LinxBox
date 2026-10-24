#include "sdkconfig.h"
#if CONFIG_LINX_VOICE_ENABLE
#include "linx_device.h"
#endif
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_pins.h"
#include "key_guard.h"
#include "linx_input.h"
#include "linx_ui.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <stdatomic.h>

static const char *TAG = "linx";
typedef struct {
    bsp_btn_t key;
    bsp_btn_ev_t event;
    uint64_t at_ms;
} input_event_t;
static QueueHandle_t s_input_queue;
static volatile bool s_input_ready;
static atomic_bool s_input_overflow;
static key_guard_t s_key_guard;

/* The shared button timer only queues events; the application task owns UI state. */
static void on_key(bsp_btn_t key, bsp_btn_ev_t event, void *user)
{
    (void)user;
    if (key_guard_consume(&s_key_guard, event == BSP_BTN_RELEASE, esp_timer_get_time())) return;
    if (!s_input_ready || !s_input_queue) return;
    if (event != BSP_BTN_PRESS && event != BSP_BTN_RELEASE && event != BSP_BTN_LONG) return;
    const input_event_t input = { key, event, (uint64_t)esp_timer_get_time() / 1000 };
    if (xQueueSend(s_input_queue, &input, 0) != pdTRUE) {
        atomic_store_explicit(&s_input_overflow, true, memory_order_relaxed);
    }
}

static linx_input_action_t input_action(linx_input_t *input, const input_event_t *event)
{
    const linx_input_event_t kind = event->event == BSP_BTN_PRESS ? LINX_INPUT_PRESS
                                    : event->event == BSP_BTN_LONG ? LINX_INPUT_LONG
                                    : LINX_INPUT_RELEASE;
    return linx_input_event(input, (unsigned)event->key, kind, event->at_ms);
}

static linx_ui_key_t ui_key(linx_input_action_t action)
{
    switch (action) {
    case LINX_INPUT_UP: return LINX_UI_KEY_UP;
    case LINX_INPUT_DOWN: return LINX_UI_KEY_DOWN;
    case LINX_INPUT_BACK: return LINX_UI_KEY_OK_LONG;
    default: return LINX_UI_KEY_OK;
    }
}

void app_main(void)
{
#if CONFIG_LINX_VOICE_ENABLE
    ESP_LOGI(TAG, "LinX voice connects on demand");
#else
    ESP_LOGI(TAG, "LinX warm-white offline demo; no radio, audio or battery initialization");
#endif
    const uint32_t wake_causes = esp_sleep_get_wakeup_causes();
    if (wake_causes & (1u << ESP_SLEEP_WAKEUP_GPIO)) {
        key_guard_arm(&s_key_guard, esp_timer_get_time(), 10LL * 1000 * 1000);
    }
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "Display initialization failed");
        return;
    }
    bsp_display_backlight(80);
    s_input_queue = xQueueCreate(24, sizeof(input_event_t));
    if (!s_input_queue) {
        ESP_LOGE(TAG, "Input queue allocation failed");
        return;
    }
    if (bsp_button_init(on_key, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "Button initialization failed");
        vQueueDelete(s_input_queue);
        s_input_queue = NULL;
        return;
    }
    key_guard_sync_released(&s_key_guard,
                            bsp_button_read_mv() >= BSP_BTN_MV_RELEASED_MIN);
    if (!bsp_lvgl_lock(1000)) {
        ESP_LOGE(TAG, "Display lock unavailable");
        return;
    }
    linx_ui_init(lv_screen_active(), (uint64_t)esp_timer_get_time() / 1000);
#if CONFIG_LINX_VOICE_ENABLE
    if (!linx_device_init()) ESP_LOGE(TAG, "Voice worker allocation failed");
    linx_ui_set_voice_handler(linx_device_request, NULL);
#endif
    bsp_lvgl_unlock();
    s_input_ready = true;
    linx_input_t input;
    linx_input_init(&input, BSP_BTN_LONG_PRESS_MS);
    input_event_t event;
    uint64_t last_metrics = 0;
    for (;;) {
        const bool received = xQueueReceive(s_input_queue, &event, pdMS_TO_TICKS(20)) == pdTRUE;
        linx_input_action_t action = received ? input_action(&input, &event) : LINX_INPUT_NONE;
        if (atomic_exchange_explicit(&s_input_overflow, false, memory_order_relaxed)) {
            linx_input_init(&input, BSP_BTN_LONG_PRESS_MS);
            xQueueReset(s_input_queue);
            action = LINX_INPUT_NONE;
            ESP_LOGW(TAG, "Input overflow recovered; discarded stale taps");
        }
        if (!bsp_lvgl_lock(100)) continue;
        const uint64_t now_ms = (uint64_t)esp_timer_get_time() / 1000;
        const linx_ui_page_t before_tick = linx_ui_page();
#if CONFIG_LINX_VOICE_ENABLE
        linx_voice_event_t voice_event;
        if (linx_device_poll(&voice_event)) linx_ui_voice_event(&voice_event);
#endif
        linx_ui_tick(now_ms);
        if (linx_ui_page() != before_tick) {
            linx_input_barrier(&input, now_ms);
            action = LINX_INPUT_NONE;
        }
        if (action != LINX_INPUT_NONE) {
            const linx_ui_page_t previous = linx_ui_page();
            linx_ui_handle_key(ui_key(action));
            if (linx_ui_page() != previous) {
                /* Old queued taps cannot click through a newly opened page. */
                linx_input_barrier(&input, (uint64_t)esp_timer_get_time() / 1000);
            }
        }
        bsp_lvgl_unlock();
        if (now_ms - last_metrics >= 30000) {
            last_metrics = now_ms;
            ESP_LOGI(TAG, "heap free=%u min=%u largest=%u stack=%u",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
        }
    }
}
