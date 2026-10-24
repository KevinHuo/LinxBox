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

void app_main(void);

enum failure { NO_FAILURE, DISPLAY_FAILURE, LVGL_FAILURE, QUEUE_FAILURE,
               BUTTON_FAILURE, INITIAL_LOCK_FAILURE };
enum scenario {
    DISPLAY_FAIL, LVGL_FAIL, QUEUE_FAIL, BUTTON_FAIL, INITIAL_LOCK_FAIL,
    IDLE_METRICS, LOOP_LOCK_FAIL, FILTER_EVENTS, BUTTON_ACTIONS,
    WAKE_HELD, WAKE_RELEASED, WAKE_TIMEOUT, INPUT_OVERFLOW,
    AUTOMATIC_BARRIER, MANUAL_BARRIER, SCENARIO_COUNT
};
static const char *const scenario_names[SCENARIO_COUNT] = {
    "display_fail", "lvgl_fail", "queue_fail", "button_fail", "initial_lock_fail",
    "idle_metrics", "loop_lock_fail", "filter_events", "button_actions",
    "wake_held", "wake_released", "wake_timeout", "input_overflow",
    "automatic_barrier", "manual_barrier"
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
static unsigned ui_init_calls, ui_ticks, automatic_changes, manual_changes;
static unsigned info_logs, error_logs, warning_logs, metric_logs;
static unsigned free_calls, minimum_calls, largest_calls, stack_calls;

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
        event_step(400, BSP_BTN_OK, BSP_BTN_PRESS);
        event_step(900, BSP_BTN_OK, BSP_BTN_LONG);
        event_step(950, BSP_BTN_OK, BSP_BTN_RELEASE);
        event_step(1000, BSP_BTN_OK, BSP_BTN_PRESS);
        event_step(1500, BSP_BTN_OK, BSP_BTN_RELEASE);
        expected_sends = 11;
        expect_key(LINX_UI_KEY_UP, 100); expect_key(LINX_UI_KEY_DOWN, 200);
        expect_key(LINX_UI_KEY_OK, 350); expect_key(LINX_UI_KEY_OK_LONG, 900);
        expect_key(LINX_UI_KEY_OK_LONG, 1500);
        break;
    case WAKE_HELD:
        wake_causes = 1U << ESP_SLEEP_WAKEUP_GPIO;
        button_mv = 100;
        event_step(100, BSP_BTN_OK, BSP_BTN_PRESS);
        event_step(600, BSP_BTN_OK, BSP_BTN_LONG);
        event_step(700, BSP_BTN_OK, BSP_BTN_RELEASE);
        event_step(800, BSP_BTN_OK, BSP_BTN_PRESS);
        event_step(900, BSP_BTN_OK, BSP_BTN_RELEASE);
        expected_sends = 2;
        expect_key(LINX_UI_KEY_OK, 900);
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
        step_t *events = step(100);
        for (unsigned index = 0; index < 25; ++index) {
            add_event(events, BSP_BTN_UP, BSP_BTN_PRESS, 100);
        }
        event_step(200, BSP_BTN_OK, BSP_BTN_RELEASE);
        event_step(300, BSP_BTN_OK, BSP_BTN_PRESS);
        event_step(350, BSP_BTN_OK, BSP_BTN_RELEASE);
        expected_sends = 28;
        expected_warnings = 1;
        expect_key(LINX_UI_KEY_OK, 350);
        break;
    }
    case AUTOMATIC_BARRIER: {
        step_t *events = step(200);
        add_event(events, BSP_BTN_UP, BSP_BTN_PRESS, 100);
        add_event(events, BSP_BTN_UP, BSP_BTN_RELEASE, 120);
        events->automatic_change = true;
        step(220);
        event_step(300, BSP_BTN_OK, BSP_BTN_PRESS);
        step(400)->automatic_change = true;
        event_step(500, BSP_BTN_OK, BSP_BTN_RELEASE);
        event_step(600, BSP_BTN_OK, BSP_BTN_PRESS);
        event_step(650, BSP_BTN_OK, BSP_BTN_RELEASE);
        expected_sends = 6;
        expect_key(LINX_UI_KEY_OK, 650);
        break;
    }
    case MANUAL_BARRIER: {
        step_t *events = step(160);
        add_event(events, BSP_BTN_OK, BSP_BTN_PRESS, 100);
        add_event(events, BSP_BTN_OK, BSP_BTN_RELEASE, 150);
        add_event(events, BSP_BTN_UP, BSP_BTN_PRESS, 155);
        add_event(events, BSP_BTN_UP, BSP_BTN_RELEASE, 157);
        step(180)->manual_change = true;
        step(200); step(220);
        event_step(300, BSP_BTN_UP, BSP_BTN_PRESS);
        steps[step_count - 1].manual_change = true;
        event_step(301, BSP_BTN_UP, BSP_BTN_PRESS);
        event_step(320, BSP_BTN_UP, BSP_BTN_RELEASE);
        expected_sends = 7;
        expect_key(LINX_UI_KEY_OK, 180); expect_key(LINX_UI_KEY_UP, 300);
        expect_key(LINX_UI_KEY_UP, 301);
        break;
    }
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

BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t wait)
{
    assert(queue == &queue_storage && queue->allocated && item != NULL);
    assert(wait == pdMS_TO_TICKS(20) && !locked);
    /* 只在模拟的阻塞队列边界终止，不向业务代码注入退出分支。 */
    if (steps_started == step_count) longjmp(loop_finished, 1);
    active_step = &steps[steps_started++];
    ++receive_calls;
    for (size_t index = 0; index < active_step->event_count; ++index) {
        const button_event_t *event = &active_step->events[index];
        assert(event->at_ms <= active_step->now_ms);
        clock_ms = event->at_ms;
        button_callback(event->key, event->event, button_user);
    }
    clock_ms = active_step->now_ms;
    if (queue->count == 0) return pdFALSE;
    memcpy(item, queue->items[queue->head], queue->item_size);
    queue->head = (queue->head + 1) % 24;
    --queue->count;
    return pdTRUE;
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
        ++loop_locks;
        if (active_step->fail_lock) {
            ++failed_loop_locks;
            return false;
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

linx_ui_page_t linx_ui_page(void)
{
    assert(locked);
    return page;
}

static void change_page(void)
{
    page = page == LINX_UI_TASKS ? LINX_UI_OVERVIEW : LINX_UI_TASKS;
}

void linx_ui_tick(uint64_t now_ms)
{
    assert(locked && active_step != NULL && now_ms == active_step->now_ms);
    ++ui_ticks;
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
    assert(strcmp(tag, "linx") == 0);
    char message[160];
    va_list arguments;
    va_start(arguments, format);
    int length = vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    assert(length >= 0 && (size_t)length < sizeof(message));
    if (level == 'I') {
        ++info_logs;
        if (strncmp(message, "heap free=", 10) == 0) {
            ++metric_logs;
            assert(strcmp(message, "heap free=65536 min=32768 largest=16384 stack=2048") == 0);
        } else {
            assert(strstr(message, "LinX warm-white offline demo") != NULL);
        }
    } else if (level == 'W') {
        ++warning_logs;
        assert(strcmp(message, "Input overflow recovered; discarded stale taps") == 0);
    } else {
        ++error_logs;
        assert(level == 'E');
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
    assert(error_logs == (failure == NO_FAILURE ? 0U : 1U));
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
    assert(initial_locks == (button_ready ? 1U : 0U));
    assert(ui_init_calls == (failure == NO_FAILURE ? 1U : 0U));
    assert(send_calls == expected_sends);
    assert(send_failures == (scenario == INPUT_OVERFLOW ? 1U : 0U));
    assert(queue_reset_calls == (scenario == INPUT_OVERFLOW ? 1U : 0U));
    assert(receive_calls == step_count && loop_locks == step_count);
    assert(failed_loop_locks == (scenario == LOOP_LOCK_FAIL ? 1U : 0U));
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
