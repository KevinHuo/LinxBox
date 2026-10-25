/* 真实设备入口的运行测试；每个场景独立进程，只启动一次 app_main。 */
#include <assert.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_pins.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "linx_ui.h"
#include "linx_input_settings.h"
#include "sdkconfig.h"
#if CONFIG_LINX_GATEWAY_ENABLE
#include "linx_gateway_transport.h"
#endif
#if CONFIG_LINX_VOICE_ENABLE || CONFIG_LINX_GATEWAY_ENABLE
#include "linx_device.h"
#endif

void app_main(void);

enum failure { NO_FAILURE, DISPLAY_FAILURE, LVGL_FAILURE, QUEUE_FAILURE,
               BUTTON_FAILURE, INITIAL_LOCK_FAILURE };
enum scenario {
    DISPLAY_FAIL, LVGL_FAIL, QUEUE_FAIL, BUTTON_FAIL, INITIAL_LOCK_FAIL,
    IDLE_METRICS, LOOP_LOCK_FAIL, FILTER_EVENTS, BUTTON_ACTIONS,
    WAKE_HELD, WAKE_RELEASED, WAKE_TIMEOUT, INPUT_OVERFLOW,
    AUTOMATIC_BARRIER, MANUAL_BARRIER, QUEUED_DOUBLE, DEFERRED_LOCK,
    LOCK_QUEUED_DOUBLE, DOUBLE_LOCK_FAIL,
    DEFAULT_LONG, SETTINGS_INIT_FAIL, SETTINGS_MODE_SWITCH, SETTINGS_SAVE_FAIL,
    MANUAL_MODE_SWITCH, MODE_SWITCH_HELD, SETTINGS_ACK_QUEUED,
#if CONFIG_LINX_VOICE_ENABLE || CONFIG_LINX_GATEWAY_ENABLE
    VOICE_INIT_FAIL, VOICE_EVENT_BARRIER,
#endif
#if CONFIG_LINX_GATEWAY_ENABLE
    GATEWAY_INIT_FAIL, GATEWAY_EVENTS, GATEWAY_EVENT_BARRIER,
#endif
    SCENARIO_COUNT
};
static const char *const scenario_names[SCENARIO_COUNT] = {
    "display_fail", "lvgl_fail", "queue_fail", "button_fail", "initial_lock_fail",
    "idle_metrics", "loop_lock_fail", "filter_events", "button_actions",
    "wake_held", "wake_released", "wake_timeout", "input_overflow",
    "automatic_barrier", "manual_barrier", "queued_double", "deferred_lock",
    "lock_queued_double", "double_lock_fail",
    "default_long", "settings_init_fail", "settings_mode_switch", "settings_save_fail",
    "manual_mode_switch", "mode_switch_held", "settings_ack_queued",
#if CONFIG_LINX_VOICE_ENABLE || CONFIG_LINX_GATEWAY_ENABLE
    "voice_init_fail", "voice_event_barrier",
#endif
#if CONFIG_LINX_GATEWAY_ENABLE
    "gateway_init_fail", "gateway_events", "gateway_event_barrier",
#endif
};

typedef struct {
    bsp_btn_t key;
    bsp_btn_ev_t event;
    uint64_t at_ms;
} button_event_t;
typedef struct {
    uint64_t now_ms;
    button_event_t events[25];
    size_t event_count;
    bool fail_lock;
    bool automatic_change;
    bool manual_change;
    bool voice_change;
    bool gateway_change;
    bool double_during_lock;
    bool settings_result;
    bool settings_saved;
    bool settings_events_during_ack;
    bool manual_mode_change;
    linx_back_mode_t settings_mode;
} step_t;

/* 队列容量遵循真实入口的 24 项；不假设私有事件结构的布局。 */
struct device_test_queue {
    unsigned char items[24][64];
    size_t item_size;
    size_t head;
    size_t count;
    bool allocated;
};
static struct device_test_queue queue_storage;
static step_t steps[32];
static size_t step_count;
static size_t steps_started;
static step_t *active_step;
static jmp_buf loop_finished;
static enum failure failure;
static uint32_t wake_causes;
static int button_mv = BSP_BTN_MV_RELEASED_MIN;
static uint64_t clock_ms;
static bsp_btn_cb_t button_callback;
static void *button_user;
static bool locked;
static lv_obj_t screen = { 1 };
static struct _lv_display_t display = { 2 };
static linx_ui_page_t page = LINX_UI_BOOT;
static linx_ui_key_t dispatched[16];
static uint64_t dispatched_at_ms[16];
static size_t dispatch_count;
static linx_ui_key_t expected_keys[16];
static uint64_t expected_at_ms[16];
static size_t expected_key_count;
static size_t expected_sends;
static unsigned expected_warnings;
static unsigned expected_metrics;
static unsigned display_calls, lvgl_calls, backlight_calls, queue_create_calls;
static unsigned button_calls, read_mv_calls, queue_delete_calls, queue_reset_calls;
static unsigned send_calls, send_failures, receive_calls;
static unsigned initial_locks, loop_locks, failed_loop_locks, unlocks;
static bool retry_lock;
static unsigned lock_retries;
static unsigned ui_init_calls, ui_ticks, automatic_changes, manual_changes;
/* 原有双击场景模拟已保存的偏好；新增场景单独验证出厂默认长按。 */
static linx_back_mode_t stored_mode = LINX_BACK_DOUBLE_CLICK;
static linx_back_mode_t ui_back_mode = LINX_BACK_LONG_PRESS;
static bool settings_init_fail;
static unsigned settings_inits, settings_binds, settings_polls, settings_results;
static unsigned info_logs, error_logs, warning_logs, metric_logs;
static unsigned free_calls, minimum_calls, largest_calls, stack_calls;
#if CONFIG_LINX_VOICE_ENABLE || CONFIG_LINX_GATEWAY_ENABLE
static bool voice_init_fail;
static unsigned voice_inits, voice_binds, voice_polls, voice_events;
#endif

#if CONFIG_LINX_GATEWAY_ENABLE
static linx_gateway_client_t *gateway_client;
static bool gateway_init_fail, gateway_ui_change;
static unsigned gateway_inits, gateway_binds, gateway_polls, gateway_ticks;
static unsigned gateway_connected, gateway_disconnected, gateway_errors, gateway_frames, gateway_releases;
static unsigned gateway_event_index;
static enum scenario active_scenario;
static unsigned random_calls;

uint32_t esp_random(void)
{
    assert(!locked);
    return ++random_calls;
}
#endif

static step_t *step(uint64_t now_ms)
{
    assert(step_count < sizeof(steps) / sizeof(steps[0]));
    step_t *result = &steps[step_count++];
    result->now_ms = now_ms;
    return result;
}

static void add_event(step_t *target, bsp_btn_t key, bsp_btn_ev_t event, uint64_t at_ms)
{
    assert(target->event_count < sizeof(target->events) / sizeof(target->events[0]));
    target->events[target->event_count++] = (button_event_t){ key, event, at_ms };
}

static void event_step(uint64_t now_ms, bsp_btn_t key, bsp_btn_ev_t event)
{
    add_event(step(now_ms), key, event, now_ms);
}

static void expect_key(linx_ui_key_t key, uint64_t at_ms)
{
    assert(expected_key_count < sizeof(expected_keys) / sizeof(expected_keys[0]));
    expected_keys[expected_key_count] = key;
    expected_at_ms[expected_key_count++] = at_ms;
}

static void configure(enum scenario scenario)
{
#if CONFIG_LINX_GATEWAY_ENABLE
    active_scenario = scenario;
#endif
    switch (scenario) {
    case DISPLAY_FAIL: failure = DISPLAY_FAILURE; break;
    case LVGL_FAIL: failure = LVGL_FAILURE; break;
    case QUEUE_FAIL: failure = QUEUE_FAILURE; break;
    case BUTTON_FAIL: failure = BUTTON_FAILURE; break;
    case INITIAL_LOCK_FAIL: failure = INITIAL_LOCK_FAILURE; break;
    case IDLE_METRICS:
        step(0); step(29999); step(30000); step(30001); step(60000);
        expected_metrics = 2;
        break;
    case LOOP_LOCK_FAIL:
        step(30000)->fail_lock = true;
        step(30001);
        expected_metrics = 1;
        break;
    case FILTER_EVENTS: {
        step_t *events = step(100);
        add_event(events, BSP_BTN_OK, BSP_BTN_CLICK, 90);
        add_event(events, BSP_BTN_OK, BSP_BTN_DOUBLE, 95);
        add_event(events, (bsp_btn_t)99, BSP_BTN_PRESS, 100);
        expected_sends = 1;
        break;
    }
    case BUTTON_ACTIONS:
        event_step(100, BSP_BTN_UP, BSP_BTN_PRESS);
        event_step(120, BSP_BTN_UP, BSP_BTN_RELEASE);
        event_step(200, BSP_BTN_DOWN, BSP_BTN_PRESS);
        event_step(220, BSP_BTN_DOWN, BSP_BTN_RELEASE);
        event_step(300, BSP_BTN_OK, BSP_BTN_PRESS);
        event_step(350, BSP_BTN_OK, BSP_BTN_RELEASE);
        step(650); step(651);
        event_step(800, BSP_BTN_OK, BSP_BTN_PRESS);
        event_step(850, BSP_BTN_OK, BSP_BTN_RELEASE);
        event_step(950, BSP_BTN_OK, BSP_BTN_PRESS);
        event_step(1000, BSP_BTN_OK, BSP_BTN_RELEASE);
        event_step(1200, BSP_BTN_OK, BSP_BTN_PRESS);
        event_step(1700, BSP_BTN_OK, BSP_BTN_LONG);
        event_step(1750, BSP_BTN_OK, BSP_BTN_RELEASE);
        step(2050); step(2051);
        expected_sends = 13;
        expect_key(LINX_UI_KEY_UP, 100); expect_key(LINX_UI_KEY_DOWN, 200);
        expect_key(LINX_UI_KEY_OK, 651); expect_key(LINX_UI_KEY_BACK, 1000);
        break;
    case WAKE_HELD:
        wake_causes = 1U << ESP_SLEEP_WAKEUP_GPIO;
        button_mv = 100;
        event_step(100, BSP_BTN_OK, BSP_BTN_PRESS);
        event_step(600, BSP_BTN_OK, BSP_BTN_LONG);
        event_step(700, BSP_BTN_OK, BSP_BTN_RELEASE);
        event_step(800, BSP_BTN_OK, BSP_BTN_PRESS);
        event_step(900, BSP_BTN_OK, BSP_BTN_RELEASE);
        step(1201);
        expected_sends = 2;
        expect_key(LINX_UI_KEY_OK, 1201);
        break;
    case WAKE_RELEASED:
        wake_causes = 1U << ESP_SLEEP_WAKEUP_GPIO;
        event_step(100, BSP_BTN_UP, BSP_BTN_PRESS);
        expected_sends = 1;
        expect_key(LINX_UI_KEY_UP, 100);
        break;
    case WAKE_TIMEOUT:
        wake_causes = 1U << ESP_SLEEP_WAKEUP_GPIO;
        button_mv = -1;
        event_step(9999, BSP_BTN_OK, BSP_BTN_LONG);
        event_step(10000, BSP_BTN_UP, BSP_BTN_PRESS);
        event_step(10001, BSP_BTN_UP, BSP_BTN_PRESS);
        event_step(10002, BSP_BTN_UP, BSP_BTN_RELEASE);
        expected_sends = 2;
        expect_key(LINX_UI_KEY_UP, 10001);
        break;
    case INPUT_OVERFLOW: {
        event_step(50, BSP_BTN_OK, BSP_BTN_PRESS);
        event_step(60, BSP_BTN_OK, BSP_BTN_RELEASE);
        step_t *events = step(100);
        for (unsigned index = 0; index < 25; ++index) {
            add_event(events, BSP_BTN_UP, BSP_BTN_PRESS, 100);
        }
        event_step(200, BSP_BTN_OK, BSP_BTN_RELEASE);
        event_step(300, BSP_BTN_OK, BSP_BTN_PRESS);
        event_step(350, BSP_BTN_OK, BSP_BTN_RELEASE);
        step(651);
        expected_sends = 30;
        expected_warnings = 1;
        expect_key(LINX_UI_KEY_OK, 651);
        break;
    }
    case AUTOMATIC_BARRIER: {
        step_t *events = step(200);
        add_event(events, BSP_BTN_UP, BSP_BTN_PRESS, 100);
        add_event(events, BSP_BTN_UP, BSP_BTN_RELEASE, 120);
        events->automatic_change = true;
        step(220);
        event_step(300, BSP_BTN_OK, BSP_BTN_PRESS);
        event_step(350, BSP_BTN_OK, BSP_BTN_RELEASE);
        step(700)->automatic_change = true;
        event_step(800, BSP_BTN_OK, BSP_BTN_PRESS);
        event_step(850, BSP_BTN_OK, BSP_BTN_RELEASE);
        step(1151);
        expected_sends = 6;
        expect_key(LINX_UI_KEY_OK, 1151);
        break;
    }
    case MANUAL_BARRIER: {
        step_t *events = step(160);
        add_event(events, BSP_BTN_OK, BSP_BTN_PRESS, 100);
        add_event(events, BSP_BTN_OK, BSP_BTN_RELEASE, 150);
        step(180);
        step(451)->manual_change = true;
        events = step(470);
        add_event(events, BSP_BTN_UP, BSP_BTN_PRESS, 440);
        add_event(events, BSP_BTN_UP, BSP_BTN_RELEASE, 445);
        step(480);
        event_step(600, BSP_BTN_UP, BSP_BTN_PRESS);
        steps[step_count - 1].manual_change = true;
        event_step(601, BSP_BTN_UP, BSP_BTN_PRESS);
        event_step(620, BSP_BTN_UP, BSP_BTN_RELEASE);
        expected_sends = 7;
        expect_key(LINX_UI_KEY_OK, 451); expect_key(LINX_UI_KEY_UP, 600);
        expect_key(LINX_UI_KEY_UP, 601);
        break;
    }
    case QUEUED_DOUBLE: {
        /* 主循环落后于按键时间时，必须先处理完队列，再派发到期单击。 */
        step_t *events = step(1000);
        add_event(events, BSP_BTN_OK, BSP_BTN_PRESS, 100);
        add_event(events, BSP_BTN_OK, BSP_BTN_RELEASE, 140);
        add_event(events, BSP_BTN_OK, BSP_BTN_PRESS, 200);
        add_event(events, BSP_BTN_OK, BSP_BTN_RELEASE, 240);
        step(1001); step(1002); step(1003); step(1004);
        expected_sends = 4;
        expect_key(LINX_UI_KEY_BACK, 1003);
        break;
    }
    case DEFERRED_LOCK: {
        event_step(100, BSP_BTN_OK, BSP_BTN_PRESS);
        event_step(150, BSP_BTN_OK, BSP_BTN_RELEASE);
        step(451)->fail_lock = true;
        step(452);
        expected_sends = 2;
        expect_key(LINX_UI_KEY_OK, 452);
        break;
    }
    case LOCK_QUEUED_DOUBLE:
        event_step(50, BSP_BTN_OK, BSP_BTN_PRESS);
        event_step(100, BSP_BTN_OK, BSP_BTN_RELEASE);
        step(350)->double_during_lock = true;
        step(470); step(490); step(510);
        expected_sends = 4;
        expect_key(LINX_UI_KEY_BACK, 490);
        break;
    case DOUBLE_LOCK_FAIL:
        event_step(100, BSP_BTN_OK, BSP_BTN_PRESS);
        event_step(140, BSP_BTN_OK, BSP_BTN_RELEASE);
        event_step(200, BSP_BTN_OK, BSP_BTN_PRESS);
        event_step(240, BSP_BTN_OK, BSP_BTN_RELEASE);
        steps[step_count - 1].fail_lock = true;
        step(260); step(280);
        expected_sends = 4;
        expect_key(LINX_UI_KEY_BACK, 260);
        break;
    case DEFAULT_LONG:
        stored_mode = LINX_BACK_LONG_PRESS;
        event_step(100, BSP_BTN_OK, BSP_BTN_PRESS);
        event_step(150, BSP_BTN_OK, BSP_BTN_RELEASE);
        event_step(200, BSP_BTN_OK, BSP_BTN_PRESS);
        event_step(700, BSP_BTN_OK, BSP_BTN_LONG);
        event_step(750, BSP_BTN_OK, BSP_BTN_LONG);
        event_step(800, BSP_BTN_OK, BSP_BTN_RELEASE);
        event_step(900, BSP_BTN_OK, BSP_BTN_PRESS);
        event_step(950, BSP_BTN_OK, BSP_BTN_RELEASE);
        expected_sends = 8;
        expect_key(LINX_UI_KEY_OK, 150); expect_key(LINX_UI_KEY_BACK, 700); expect_key(LINX_UI_KEY_OK, 950);
        break;
    case SETTINGS_INIT_FAIL:
        settings_init_fail = true;
        event_step(100, BSP_BTN_OK, BSP_BTN_PRESS);
        event_step(150, BSP_BTN_OK, BSP_BTN_RELEASE);
        expected_sends = 2; expected_warnings = 1;
        expect_key(LINX_UI_KEY_OK, 150);
        break;
    case SETTINGS_MODE_SWITCH: {
        event_step(100, BSP_BTN_OK, BSP_BTN_PRESS);
        event_step(150, BSP_BTN_OK, BSP_BTN_RELEASE);
        step_t *change = step(200);
        add_event(change, BSP_BTN_UP, BSP_BTN_PRESS, 190);
        add_event(change, BSP_BTN_UP, BSP_BTN_RELEASE, 195);
        add_event(change, BSP_BTN_OK, BSP_BTN_PRESS, 196);
        add_event(change, BSP_BTN_OK, BSP_BTN_RELEASE, 197);
        change->settings_result = change->settings_saved = true;
        change->settings_mode = LINX_BACK_LONG_PRESS;
        step(210); step(220); step(230);
        event_step(300, BSP_BTN_OK, BSP_BTN_PRESS);
        event_step(350, BSP_BTN_OK, BSP_BTN_RELEASE);
        step(700);
        expected_sends = 8;
        expect_key(LINX_UI_KEY_OK, 350);
        break;
    }
    case SETTINGS_SAVE_FAIL: {
        event_step(100, BSP_BTN_OK, BSP_BTN_PRESS);
        event_step(150, BSP_BTN_OK, BSP_BTN_RELEASE);
        step_t *failed = step(200);
        failed->settings_result = true; failed->settings_mode = LINX_BACK_LONG_PRESS;
        step(451);
        event_step(500, BSP_BTN_OK, BSP_BTN_PRESS);
        event_step(550, BSP_BTN_OK, BSP_BTN_RELEASE);
        event_step(600, BSP_BTN_OK, BSP_BTN_PRESS);
        event_step(650, BSP_BTN_OK, BSP_BTN_RELEASE);
        expected_sends = 6;
        expect_key(LINX_UI_KEY_OK, 451); expect_key(LINX_UI_KEY_BACK, 650);
        break;
    }
    case MANUAL_MODE_SWITCH: {
        stored_mode = LINX_BACK_LONG_PRESS;
        event_step(100, BSP_BTN_OK, BSP_BTN_PRESS);
        step_t *change = step(150);
        add_event(change, BSP_BTN_OK, BSP_BTN_RELEASE, 150);
        add_event(change, BSP_BTN_OK, BSP_BTN_PRESS, 145);
        add_event(change, BSP_BTN_OK, BSP_BTN_RELEASE, 149);
        change->manual_mode_change = true;
        step(160); step(170);
        event_step(200, BSP_BTN_OK, BSP_BTN_PRESS);
        event_step(250, BSP_BTN_OK, BSP_BTN_RELEASE);
        event_step(300, BSP_BTN_OK, BSP_BTN_PRESS);
        event_step(350, BSP_BTN_OK, BSP_BTN_RELEASE);
        expected_sends = 8;
        expect_key(LINX_UI_KEY_OK, 150); expect_key(LINX_UI_KEY_BACK, 350);
        break;
    }
    case SETTINGS_ACK_QUEUED: {
        step_t *change = step(200);
        change->settings_result = change->settings_saved = true;
        change->settings_mode = LINX_BACK_LONG_PRESS;
        change->settings_events_during_ack = true;
        step(300); step(320);
        event_step(400, BSP_BTN_OK, BSP_BTN_PRESS);
        event_step(450, BSP_BTN_OK, BSP_BTN_RELEASE);
        expected_sends = 4;
        expect_key(LINX_UI_KEY_OK, 450);
        break;
    }
    case MODE_SWITCH_HELD: {
        event_step(100, BSP_BTN_OK, BSP_BTN_PRESS);
        step_t *change = step(200);
        change->settings_result = change->settings_saved = true; change->settings_mode = LINX_BACK_LONG_PRESS;
        event_step(300, BSP_BTN_OK, BSP_BTN_RELEASE);
        event_step(400, BSP_BTN_OK, BSP_BTN_PRESS);
        event_step(450, BSP_BTN_OK, BSP_BTN_RELEASE);
        expected_sends = 4;
        expect_key(LINX_UI_KEY_OK, 450);
        break;
    }
#if CONFIG_LINX_VOICE_ENABLE || CONFIG_LINX_GATEWAY_ENABLE
    case VOICE_INIT_FAIL:
        voice_init_fail = true;
        step(100);
        break;
    case VOICE_EVENT_BARRIER:
        event_step(100, BSP_BTN_OK, BSP_BTN_PRESS);
        step(200)->voice_change = true;
        event_step(300, BSP_BTN_OK, BSP_BTN_RELEASE);
        event_step(400, BSP_BTN_OK, BSP_BTN_PRESS);
        event_step(450, BSP_BTN_OK, BSP_BTN_RELEASE);
        step(751);
        expected_sends = 4;
        expect_key(LINX_UI_KEY_OK, 751);
        break;
#endif
#if CONFIG_LINX_GATEWAY_ENABLE
    case GATEWAY_INIT_FAIL:
        gateway_init_fail = true;
        step(100);
        break;
    case GATEWAY_EVENTS:
        step(100)->gateway_change = true;
        step(200)->gateway_change = true;
        break;
    case GATEWAY_EVENT_BARRIER:
        event_step(100, BSP_BTN_OK, BSP_BTN_PRESS);
        step(200)->gateway_change = true;
        event_step(300, BSP_BTN_OK, BSP_BTN_RELEASE);
        event_step(400, BSP_BTN_OK, BSP_BTN_PRESS);
        event_step(450, BSP_BTN_OK, BSP_BTN_RELEASE);
        step(751);
        expected_sends = 4;
        expect_key(LINX_UI_KEY_OK, 751);
        break;
#endif
    case SCENARIO_COUNT: assert(false); break;
    }
}

esp_err_t bsp_display_init(void)
{
    ++display_calls;
    return failure == DISPLAY_FAILURE ? ESP_FAIL : ESP_OK;
}

struct _lv_display_t *bsp_lvgl_init(void)
{
    assert(display_calls == 1);
    ++lvgl_calls;
    return failure == LVGL_FAILURE ? NULL : &display;
}

void bsp_display_backlight(uint8_t percent)
{
    assert(percent == 80 && lvgl_calls == 1);
    ++backlight_calls;
}

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size)
{
    assert(length == 24 && item_size > 0 && item_size <= sizeof(queue_storage.items[0]));
    assert(backlight_calls == 1);
    ++queue_create_calls;
    if (failure == QUEUE_FAILURE) return NULL;
    queue_storage.item_size = item_size;
    queue_storage.allocated = true;
    return &queue_storage;
}

esp_err_t bsp_button_init(bsp_btn_cb_t callback, void *user)
{
    assert(queue_storage.allocated && callback != NULL && user == NULL);
    ++button_calls;
    button_callback = callback;
    button_user = user;
    /* 驱动注册期间的回调不得抢在 UI 就绪前把事件交给应用。 */
    callback(BSP_BTN_OK, BSP_BTN_PRESS, user);
    assert(send_calls == 0);
    return failure == BUTTON_FAILURE ? ESP_FAIL : ESP_OK;
}

int bsp_button_read_mv(void)
{
    assert(button_calls == 1 && failure != BUTTON_FAILURE);
    ++read_mv_calls;
    return button_mv;
}

void vQueueDelete(QueueHandle_t queue)
{
    assert(queue == &queue_storage && queue->allocated);
    queue->allocated = false;
    ++queue_delete_calls;
}

BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t wait)
{
    assert(queue == &queue_storage && queue->allocated && item != NULL && wait == 0);
    ++send_calls;
    if (queue->count == 24) {
        ++send_failures;
        return pdFALSE;
    }
    memcpy(queue->items[(queue->head + queue->count) % 24], item, queue->item_size);
    ++queue->count;
    return pdTRUE;
}

BaseType_t xQueueReset(QueueHandle_t queue)
{
    assert(queue == &queue_storage && queue->allocated);
    assert(queue->count == 23);
    queue->head = queue->count = 0;
    ++queue_reset_calls;
    return pdTRUE;
}

static void begin_step(void)
{
    /* 只在模拟的队列等待或锁重试边界终止，不向业务代码注入退出分支。 */
    if (steps_started == step_count) longjmp(loop_finished, 1);
    active_step = &steps[steps_started++];
    for (size_t index = 0; index < active_step->event_count; ++index) {
        const button_event_t *event = &active_step->events[index];
        assert(event->at_ms <= active_step->now_ms);
        clock_ms = event->at_ms;
        button_callback(event->key, event->event, button_user);
    }
    clock_ms = active_step->now_ms;
}

BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t wait)
{
    assert(queue == &queue_storage && queue->allocated && item != NULL);
    assert(wait == pdMS_TO_TICKS(20) && !locked);
    retry_lock = false;
    begin_step();
    ++receive_calls;
    if (queue->count == 0) return pdFALSE;
    memcpy(item, queue->items[queue->head], queue->item_size);
    queue->head = (queue->head + 1) % 24;
    --queue->count;
    return pdTRUE;
}

UBaseType_t uxQueueMessagesWaiting(const QueueHandle_t queue)
{
    assert(queue == &queue_storage && queue->allocated && locked);
    return (UBaseType_t)queue->count;
}

bool bsp_lvgl_lock(int timeout_ms)
{
    assert(!locked);
    if (timeout_ms == 1000) {
        assert(initial_locks == 0);
        ++initial_locks;
        if (failure == INITIAL_LOCK_FAILURE) return false;
    } else {
        assert(timeout_ms == 100 && active_step != NULL);
        if (retry_lock) {
            retry_lock = false;
            ++lock_retries;
            begin_step();
        }
        ++loop_locks;
        if (active_step->fail_lock) {
            ++failed_loop_locks;
            retry_lock = true;
            return false;
        }
        if (active_step->double_during_lock) {
            clock_ms = active_step->now_ms + 10;
            button_callback(BSP_BTN_OK, BSP_BTN_PRESS, button_user);
            clock_ms = active_step->now_ms + 40;
            button_callback(BSP_BTN_OK, BSP_BTN_RELEASE, button_user);
            active_step->now_ms += 100;
            clock_ms = active_step->now_ms;
        }
    }
    locked = true;
    return true;
}

void bsp_lvgl_unlock(void)
{
    assert(locked);
    locked = false;
    ++unlocks;
}

uint32_t esp_sleep_get_wakeup_causes(void) { return wake_causes; }
int64_t esp_timer_get_time(void) { return (int64_t)clock_ms * 1000; }

lv_obj_t *lv_screen_active(void)
{
    assert(locked);
    return &screen;
}

void linx_ui_init(lv_obj_t *target, uint64_t now_ms)
{
    assert(locked && target == &screen && now_ms == 0);
    ++ui_init_calls;
    page = LINX_UI_TASKS;
}

bool linx_input_settings_init(linx_back_mode_t *mode)
{
    assert(!locked && ui_init_calls == 0 && read_mv_calls == 1 && mode);
    ++settings_inits;
    *mode = settings_init_fail ? LINX_BACK_LONG_PRESS : stored_mode;
    return !settings_init_fail;
}

bool linx_input_settings_request(linx_back_mode_t mode, void *user)
{
    assert(locked && user == NULL && (mode == LINX_BACK_LONG_PRESS || mode == LINX_BACK_DOUBLE_CLICK));
    return !settings_init_fail;
}

bool linx_input_settings_poll(linx_input_settings_event_t *event)
{
    assert(locked && event && settings_binds == 1);
    ++settings_polls;
    if (!active_step->settings_result) return false;
    *event = (linx_input_settings_event_t){.mode = active_step->settings_mode, .saved = active_step->settings_saved};
    return true;
}

void linx_ui_set_back_mode(linx_back_mode_t mode)
{
    assert(locked && ui_init_calls == 1);
    ui_back_mode = mode;
}

linx_back_mode_t linx_ui_back_mode(void)
{
    assert(locked && ui_init_calls == 1);
    return ui_back_mode;
}

void linx_ui_set_back_mode_handler(linx_ui_back_mode_handler_t handler, void *user)
{
    assert(locked && handler == linx_input_settings_request && user == NULL && settings_inits == 1);
    ++settings_binds;
}

void linx_ui_back_mode_result(linx_back_mode_t mode, bool saved)
{
    assert(locked && settings_binds == 1);
    ++settings_results;
    if (saved) ui_back_mode = mode;
    if (active_step->settings_events_during_ack) {
        /* ACK 的 UI 重绘期间按键仍在排队，时间晚于主循环提前采样的 now_ms。 */
        clock_ms = active_step->now_ms + 10;
        button_callback(BSP_BTN_OK, BSP_BTN_PRESS, button_user);
        clock_ms = active_step->now_ms + 40;
        button_callback(BSP_BTN_OK, BSP_BTN_RELEASE, button_user);
        clock_ms = active_step->now_ms + 80;
    }
}

linx_ui_page_t linx_ui_page(void)
{
    assert(locked);
    return page;
}

static void change_page(void)
{
    page = page == LINX_UI_TASKS ? LINX_UI_OVERVIEW : LINX_UI_TASKS;
}

#if CONFIG_LINX_VOICE_ENABLE || CONFIG_LINX_GATEWAY_ENABLE
bool linx_device_init(void)
{
    assert(!locked && ui_init_calls == 0);
    ++voice_inits;
    return !voice_init_fail;
}

bool linx_device_request(const linx_voice_request_t *request, void *user)
{
    (void)request; (void)user;
    assert(false);
    return false;
}

void linx_ui_set_voice_handler(linx_voice_handler_t handler, void *user)
{
    assert(locked && handler == linx_device_request && user == NULL);
    assert(voice_inits == 1);
    ++voice_binds;
}

bool linx_device_poll(linx_voice_event_t *event)
{
    assert(locked && voice_binds == 1 && event != NULL);
    ++voice_polls;
    *event = (linx_voice_event_t){.generation = 42, .state = LINX_VOICE_FINISHED};
    return active_step->voice_change;
}

bool linx_ui_voice_event(const linx_voice_event_t *event)
{
    assert(locked && event->generation == 42 && event->state == LINX_VOICE_FINISHED);
    ++voice_events;
    change_page();
    return true;
}
#endif


#if CONFIG_LINX_GATEWAY_ENABLE
bool linx_gateway_transport_init(void)
{
    assert(!locked && voice_inits == 1U);
    ++gateway_inits;
    return !gateway_init_fail;
}

bool linx_gateway_transport_send(uint32_t generation, const char *json, size_t size, void *user)
{
    (void)generation; (void)json; (void)size; (void)user;
    assert(false);
    return false;
}

void linx_gateway_client_init(linx_gateway_client_t *client, const linx_gateway_client_config_t *config)
{
    assert(!locked && config->send == linx_gateway_transport_send && config->configured && config->instance_id);
    assert(config->log);
    const linx_gateway_log_t entry = {.phase = "tx_queued", .generation = 1U, .type = "request.confirm",
        .operation = "request.confirm", .size = 64U, .status = "", .error_code = ""};
    config->log(&entry, config->user);
    gateway_client = client;
    client->view.configured = config->configured;
}

void linx_ui_set_gateway_client(linx_gateway_client_t *client)
{
    assert(locked && client == gateway_client && gateway_inits == 1U);
    ++gateway_binds;
}

bool linx_gateway_transport_poll(linx_gateway_transport_event_t *event)
{
    assert(!locked && active_step);
    ++gateway_polls;
    if (!active_step->gateway_change) return NULL;
    if (active_scenario == GATEWAY_EVENT_BARRIER) {
        if (gateway_event_index++) return false;
        *event = (linx_gateway_transport_event_t){.type = LINX_GATEWAY_TRANSPORT_FRAME,
            .generation = 7U, .data = "{}", .size = 2U};
        return true;
    }
    if (gateway_event_index >= 5U) return false;
    static const linx_gateway_transport_event_type_t types[] = {LINX_GATEWAY_TRANSPORT_ERROR,
        LINX_GATEWAY_TRANSPORT_CONNECTED, LINX_GATEWAY_TRANSPORT_FRAME,
        LINX_GATEWAY_TRANSPORT_DISCONNECTED, LINX_GATEWAY_TRANSPORT_NOT_CONFIGURED};
    *event = (linx_gateway_transport_event_t){.type = types[gateway_event_index], .generation = gateway_event_index ? 7U : 0U,
        .mac_address = "AA:BB:CC:DD:EE:FF", .data = "{}", .size = 2U};
    ++gateway_event_index;
    return true;
}

void linx_gateway_client_connected(linx_gateway_client_t *client, uint32_t generation, const char *mac, uint32_t now)
{
    assert(!locked && client == gateway_client && generation == 7U && strlen(mac) == 17U && now == clock_ms);
    ++gateway_connected;
}

void linx_gateway_client_disconnected(linx_gateway_client_t *client, uint32_t generation)
{
    assert(!locked && client == gateway_client && generation == 7U);
    ++gateway_disconnected;
}

void linx_gateway_client_transport_error(linx_gateway_client_t *client, uint32_t generation)
{
    assert(!locked && client == gateway_client && generation == 0U);
    ++gateway_errors;
}

bool linx_gateway_client_receive(linx_gateway_client_t *client, uint32_t generation,
                                 const char *bytes, size_t size, uint32_t now)
{
    assert(!locked && client == gateway_client && generation == 7U && !strcmp(bytes, "{}") && size == 2U && now == clock_ms);
    ++gateway_frames;
    gateway_ui_change = active_scenario == GATEWAY_EVENT_BARRIER;
    return true;
}

void linx_gateway_transport_release(void)
{
    assert(!locked && gateway_frames == gateway_releases + 1U);
    ++gateway_releases;
}

void linx_gateway_client_tick(linx_gateway_client_t *client, uint32_t now)
{
    assert(!locked && client == gateway_client && now == clock_ms);
    ++gateway_ticks;
}
#endif

void linx_ui_tick(uint64_t now_ms)
{
    assert(locked && active_step != NULL && now_ms == active_step->now_ms);
    ++ui_ticks;
#if CONFIG_LINX_GATEWAY_ENABLE
    if (gateway_ui_change) { change_page(); gateway_ui_change = false; }
#endif
    if (active_step->automatic_change) {
        change_page();
        ++automatic_changes;
    }
}

void linx_ui_handle_key(linx_ui_key_t key)
{
    assert(locked && active_step != NULL);
    assert(dispatch_count < sizeof(dispatched) / sizeof(dispatched[0]));
    dispatched[dispatch_count] = key;
    dispatched_at_ms[dispatch_count++] = clock_ms;
    if (active_step->manual_mode_change) ui_back_mode = LINX_BACK_DOUBLE_CLICK;
    if (active_step->manual_change) {
        change_page();
        ++manual_changes;
    }
}

size_t heap_caps_get_free_size(uint32_t capabilities)
{
    assert(capabilities == MALLOC_CAP_INTERNAL && !locked);
    ++free_calls;
    return 65536;
}

size_t heap_caps_get_minimum_free_size(uint32_t capabilities)
{
    assert(capabilities == MALLOC_CAP_INTERNAL && !locked);
    ++minimum_calls;
    return 32768;
}

size_t heap_caps_get_largest_free_block(uint32_t capabilities)
{
    assert(capabilities == MALLOC_CAP_INTERNAL && !locked);
    ++largest_calls;
    return 16384;
}

UBaseType_t uxTaskGetStackHighWaterMark(void *task)
{
    assert(task == NULL && !locked);
    ++stack_calls;
    return 2048;
}

void device_test_log(char level, const char *tag, const char *format, ...)
{
    char message[160];
    va_list arguments;
    va_start(arguments, format);
    int length = vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    assert(length >= 0 && (size_t)length < sizeof(message));
#if CONFIG_LINX_GATEWAY_ENABLE
    if (!strcmp(tag, "linx_gateway")) {
        assert(level == 'I');
        assert(!strcmp(message, "protocol=tx_queued generation=1 type=request.confirm operation=request.confirm bytes=64 status= error="));
        return;
    }
#endif
    assert(strcmp(tag, "linx") == 0);
    if (level == 'I') {
        ++info_logs;
        if (strncmp(message, "heap free=", 10) == 0) {
            ++metric_logs;
            assert(strcmp(message, "heap free=65536 min=32768 largest=16384 stack=2048") == 0);
        } else {
#if CONFIG_LINX_GATEWAY_ENABLE
            assert(strcmp(message, "LinX connects to the configured Gateway") == 0);
#elif CONFIG_LINX_VOICE_ENABLE
            assert(strcmp(message, "LinX voice connects on demand") == 0);
#else
            assert(strstr(message, "LinX warm-white offline demo") != NULL);
#endif
        }
    } else if (level == 'W') {
        ++warning_logs;
        assert(strcmp(message, settings_init_fail
            ? "Input settings worker unavailable; using loaded return mode"
            : "Input overflow recovered; discarded stale taps") == 0);
    } else {
        ++error_logs;
        assert(level == 'E');
#if CONFIG_LINX_VOICE_ENABLE || CONFIG_LINX_GATEWAY_ENABLE
        if (voice_init_fail) {
            assert(strcmp(message, "Voice worker allocation failed") == 0);
            return;
        }
#endif
#if CONFIG_LINX_GATEWAY_ENABLE
        if (gateway_init_fail) { assert(strcmp(message, "Gateway worker allocation failed") == 0); return; }
#endif
        const char *expected = failure == DISPLAY_FAILURE || failure == LVGL_FAILURE
            ? "Display initialization failed" : failure == QUEUE_FAILURE
            ? "Input queue allocation failed" : failure == BUTTON_FAILURE
            ? "Button initialization failed" : "Display lock unavailable";
        assert(strcmp(message, expected) == 0);
    }
}

static void verify(enum scenario scenario)
{
    assert(display_calls == 1 && !locked);
    assert(info_logs == 1 + expected_metrics);
#if CONFIG_LINX_VOICE_ENABLE || CONFIG_LINX_GATEWAY_ENABLE
    assert(error_logs == (failure == NO_FAILURE && !voice_init_fail
#if CONFIG_LINX_GATEWAY_ENABLE
        && !gateway_init_fail
#endif
        ? 0U : 1U));
    assert(voice_inits == ui_init_calls + (failure == INITIAL_LOCK_FAILURE ? 1U : 0U));
    assert(voice_binds == ui_init_calls);
    assert(voice_polls == ui_ticks);
    assert(voice_events == (scenario == VOICE_EVENT_BARRIER ? 1U : 0U));
#else
    assert(error_logs == (failure == NO_FAILURE ? 0U : 1U));
#endif
#if CONFIG_LINX_GATEWAY_ENABLE
    assert(gateway_inits == voice_inits && gateway_binds == ui_init_calls);
    assert(gateway_ticks == loop_locks);
    assert(gateway_frames == gateway_releases);
    if (scenario == GATEWAY_EVENTS) {
        assert(gateway_connected == 1U && gateway_disconnected == 1U && gateway_errors == 1U && gateway_frames == 1U);
        assert(!gateway_client->view.configured);
    }
    if (scenario == GATEWAY_EVENT_BARRIER) assert(gateway_frames == 1U);
#endif
    assert(warning_logs == expected_warnings && metric_logs == expected_metrics);
    assert(free_calls == expected_metrics && minimum_calls == expected_metrics);
    assert(largest_calls == expected_metrics && stack_calls == expected_metrics);
    assert(lvgl_calls == (failure == DISPLAY_FAILURE ? 0U : 1U));
    const bool display_ready = failure != DISPLAY_FAILURE && failure != LVGL_FAILURE;
    assert(backlight_calls == (display_ready ? 1U : 0U));
    assert(queue_create_calls == (display_ready ? 1U : 0U));
    const bool queue_ready = display_ready && failure != QUEUE_FAILURE;
    assert(button_calls == (queue_ready ? 1U : 0U));
    assert(queue_delete_calls == (failure == BUTTON_FAILURE ? 1U : 0U));
    const bool button_ready = queue_ready && failure != BUTTON_FAILURE;
    assert(read_mv_calls == (button_ready ? 1U : 0U));
    assert(settings_inits == (button_ready ? 1U : 0U));
    assert(initial_locks == (button_ready ? 1U : 0U));
    assert(settings_binds == ui_init_calls && settings_polls == ui_ticks);
    assert(settings_results == (scenario == SETTINGS_MODE_SWITCH || scenario == SETTINGS_SAVE_FAIL || scenario == MODE_SWITCH_HELD || scenario == SETTINGS_ACK_QUEUED ? 1U : 0U));
    assert(ui_init_calls == (failure == NO_FAILURE ? 1U : 0U));
    assert(send_calls == expected_sends);
    assert(send_failures == (scenario == INPUT_OVERFLOW ? 1U : 0U));
    assert(queue_reset_calls == (scenario == INPUT_OVERFLOW ? 1U : 0U));
    assert(receive_calls + lock_retries == step_count && loop_locks == step_count);
    assert(failed_loop_locks == (scenario == LOOP_LOCK_FAIL || scenario == DEFERRED_LOCK ||
                                 scenario == DOUBLE_LOCK_FAIL ? 1U : 0U));
    assert(ui_ticks == step_count - failed_loop_locks);
    assert(unlocks == ui_init_calls + ui_ticks);
    assert(dispatch_count == expected_key_count);
    for (size_t index = 0; index < dispatch_count; ++index) {
        assert(dispatched[index] == expected_keys[index]);
        assert(dispatched_at_ms[index] == expected_at_ms[index]);
    }
    assert(automatic_changes == (scenario == AUTOMATIC_BARRIER ? 2U : 0U));
    assert(manual_changes == (scenario == MANUAL_BARRIER ? 2U : 0U));
    assert(queue_storage.count == 0);
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--list") == 0) {
        for (unsigned index = 0; index < SCENARIO_COUNT; ++index) puts(scenario_names[index]);
        return 0;
    }
    enum scenario scenario = SCENARIO_COUNT;
    for (unsigned index = 0; argc == 2 && index < SCENARIO_COUNT; ++index) {
        if (strcmp(argv[1], scenario_names[index]) == 0) scenario = (enum scenario)index;
    }
    if (scenario == SCENARIO_COUNT) {
        fprintf(stderr, "用法：%s <场景名>；--list 列出所有场景\n", argv[0]);
        return 2;
    }
    configure(scenario);
    if (setjmp(loop_finished) == 0) {
        app_main();
        assert(failure != NO_FAILURE);
    } else {
        assert(failure == NO_FAILURE && steps_started == step_count);
    }
    verify(scenario);
    printf("设备入口场景通过：%s\n", scenario_names[scenario]);
    return 0;
}
