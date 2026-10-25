#include "sdkconfig.h"
#if CONFIG_LINX_VOICE_ENABLE || CONFIG_LINX_GATEWAY_ENABLE
#include "linx_device.h"
#endif
#if CONFIG_LINX_GATEWAY_ENABLE
#include "linx_gateway_client.h"
#include "linx_gateway_transport.h"
#include "esp_random.h"
#include <string.h>
static linx_gateway_client_t s_gateway;

/* 收帧、JSON 解析与超时均在取得 LVGL 锁前完成。 */
static void gateway_poll(uint32_t now_ms)
{
    linx_gateway_transport_event_t event;
    for (unsigned i = 0U; i < 4U && linx_gateway_transport_poll(&event); ++i) {
        switch (event.type) {
        case LINX_GATEWAY_TRANSPORT_CONNECTED:
            linx_gateway_client_connected(&s_gateway, event.generation, event.mac_address, now_ms);
            break;
        case LINX_GATEWAY_TRANSPORT_FRAME:
            linx_gateway_client_receive(&s_gateway, event.generation, event.data, event.size, now_ms);
            linx_gateway_transport_release();
            break;
        case LINX_GATEWAY_TRANSPORT_NOT_CONFIGURED:
            s_gateway.view.configured = false;
            s_gateway.view.error = LINX_GATEWAY_ERROR_UNCONFIGURED;
            ++s_gateway.view.revision;
            break;
        case LINX_GATEWAY_TRANSPORT_ERROR:
            linx_gateway_client_transport_error(&s_gateway, event.generation);
            break;
        default:
            linx_gateway_client_disconnected(&s_gateway, event.generation);
            break;
        }
    }
    linx_gateway_client_tick(&s_gateway, now_ms);
}
#endif
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_pins.h"
#include "key_guard.h"
#include "linx_input.h"
#include "linx_input_settings.h"
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
#if CONFIG_LINX_GATEWAY_ENABLE
static void gateway_log(const linx_gateway_log_t *entry, void *user)
{
    (void)user;
    ESP_LOGI("linx_gateway", "protocol=%s generation=%lu type=%s operation=%s bytes=%zu status=%s error=%s",
             entry->phase, (unsigned long)entry->generation, entry->type, entry->operation,
             entry->size, entry->status, entry->error_code);
}
#endif
static const uint32_t DOUBLE_CLICK_MS = 300U;
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
    case LINX_INPUT_BACK: return LINX_UI_KEY_BACK;
    default: return LINX_UI_KEY_OK;
    }
}

void app_main(void)
{
#if CONFIG_LINX_GATEWAY_ENABLE
    ESP_LOGI(TAG, "LinX connects to the configured Gateway");
#elif CONFIG_LINX_VOICE_ENABLE
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
    linx_back_mode_t loaded_mode = LINX_BACK_LONG_PRESS;
    if (!linx_input_settings_init(&loaded_mode))
        ESP_LOGW(TAG, "Input settings worker unavailable; using loaded return mode");
#if CONFIG_LINX_VOICE_ENABLE || CONFIG_LINX_GATEWAY_ENABLE
    if (!linx_device_init()) ESP_LOGE(TAG, "Voice worker allocation failed");
#endif
#if CONFIG_LINX_GATEWAY_ENABLE
    uint64_t instance_id = ((uint64_t)esp_random() << 32U) | esp_random();
    if (instance_id == 0U) instance_id = 1U;
    const linx_gateway_client_config_t gateway_config = {
        .configured = true, .instance_id = instance_id, .send = linx_gateway_transport_send,
        .user = NULL, .log = gateway_log};
    linx_gateway_client_init(&s_gateway, &gateway_config);
    if (!linx_gateway_transport_init()) {
        ESP_LOGE(TAG, "Gateway worker allocation failed");
        s_gateway.view.error = LINX_GATEWAY_ERROR_MEMORY;
        ++s_gateway.view.revision;
    }
#endif
    if (!bsp_lvgl_lock(1000)) {
        ESP_LOGE(TAG, "Display lock unavailable");
        return;
    }
    linx_ui_init(lv_screen_active(), (uint64_t)esp_timer_get_time() / 1000);
    linx_ui_set_back_mode(loaded_mode);
    linx_ui_set_back_mode_handler(linx_input_settings_request, NULL);
    const linx_back_mode_t initial_mode = linx_ui_back_mode();
#if CONFIG_LINX_VOICE_ENABLE || CONFIG_LINX_GATEWAY_ENABLE
    linx_ui_set_voice_handler(linx_device_request, NULL);
#endif
#if CONFIG_LINX_GATEWAY_ENABLE
    linx_ui_set_gateway_client(&s_gateway);
#endif
    bsp_lvgl_unlock();
    s_input_ready = true;
    linx_input_t input;
    linx_input_init(&input, DOUBLE_CLICK_MS);
    linx_input_set_back_mode(&input, initial_mode, 0U);
    input_event_t event;
    bool pending_event = false;
    uint64_t last_metrics = 0;
    for (;;) {
        if (!pending_event) pending_event = xQueueReceive(s_input_queue, &event, pdMS_TO_TICKS(20)) == pdTRUE;
        const bool received = pending_event;
        linx_input_action_t action = LINX_INPUT_NONE;
        if (atomic_exchange_explicit(&s_input_overflow, false, memory_order_relaxed)) {
            linx_input_barrier(&input, (uint64_t)esp_timer_get_time() / 1000);
            xQueueReset(s_input_queue);
            pending_event = false;
            ESP_LOGW(TAG, "Input overflow recovered; discarded stale taps");
        }
#if CONFIG_LINX_GATEWAY_ENABLE
        gateway_poll((uint32_t)((uint64_t)esp_timer_get_time() / 1000U));
#endif
        if (!bsp_lvgl_lock(100)) continue;
        if (pending_event) action = input_action(&input, &event);
        pending_event = false;
        const uint64_t now_ms = (uint64_t)esp_timer_get_time() / 1000;
        const linx_ui_page_t before_tick = linx_ui_page();
#if CONFIG_LINX_VOICE_ENABLE || CONFIG_LINX_GATEWAY_ENABLE
        linx_voice_event_t voice_event;
        if (linx_device_poll(&voice_event)) linx_ui_voice_event(&voice_event);
#endif
        linx_input_settings_event_t settings_event;
        if (linx_input_settings_poll(&settings_event))
            linx_ui_back_mode_result(settings_event.mode, settings_event.saved);
        linx_ui_tick(now_ms);
        if (input.back_mode != linx_ui_back_mode()) {
            linx_input_set_back_mode(&input, linx_ui_back_mode(), (uint64_t)esp_timer_get_time() / 1000);
            action = LINX_INPUT_NONE;
        } else if (linx_ui_page() != before_tick) {
            linx_input_barrier(&input, now_ms);
            action = LINX_INPUT_NONE;
        } else if (!received && uxQueueMessagesWaiting(s_input_queue) == 0U) {
            /* 先处理排队的第二击；空队列时才派发到期单击。 */
            action = linx_input_tick(&input, now_ms);
        }
        if (action != LINX_INPUT_NONE) {
            const linx_ui_page_t previous = linx_ui_page();
            linx_ui_handle_key(ui_key(action));
            if (input.back_mode != linx_ui_back_mode()) {
                linx_input_set_back_mode(&input, linx_ui_back_mode(), (uint64_t)esp_timer_get_time() / 1000);
            } else if (linx_ui_page() != previous) {
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
