/* 按锁定 SDK 模拟 set 直接写入；保存错误不等于持久区已回滚。 */
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../main/linx_input_settings.c"

struct test_queue {
    size_t size;
    bool ready;
    unsigned char data[sizeof(linx_input_settings_event_t)];
};
static bool persisted_namespace, persisted_value, handle_open;
static uint8_t flash_value;
static const char *failure;
static unsigned queue_calls, queue_fail_at, queue_deletes, task_calls, commits, closes;
static bool reject_queue, skip_receive;
static void (*task_entry)(void *);
static jmp_buf task_end;

static esp_err_t status(const char *operation)
{
    return failure && strcmp(failure, operation) == 0 ? ESP_FAIL : ESP_OK;
}

esp_err_t nvs_flash_init(void) { return status("init"); }
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle)
{
    assert(strcmp(name, "linx_ui") == 0 && !handle_open);
    assert(mode == NVS_READONLY || mode == NVS_READWRITE);
    esp_err_t error = status(mode == NVS_READONLY ? "read_open" : "write_open");
    if (error != ESP_OK) return error;
    if (mode == NVS_READONLY && !persisted_namespace) return ESP_ERR_NVS_NOT_FOUND;
    handle_open = true;
    *handle = 1;
    return ESP_OK;
}
esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *value)
{
    assert(handle == 1 && handle_open && strcmp(key, "back_mode") == 0);
    if (status("get") != ESP_OK) return ESP_FAIL;
    if (!persisted_value) return ESP_ERR_NVS_NOT_FOUND;
    *value = flash_value;
    return ESP_OK;
}
esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value)
{
    assert(handle == 1 && handle_open && strcmp(key, "back_mode") == 0);
    if (status("set") != ESP_OK) return ESP_FAIL;
    persisted_namespace = persisted_value = true;
    flash_value = value;
    return status("set_after_write");
}
esp_err_t nvs_commit(nvs_handle_t handle)
{
    assert(handle == 1 && handle_open);
    ++commits;
    if (status("commit") != ESP_OK) return ESP_FAIL;
    return ESP_OK;
}
void nvs_close(nvs_handle_t handle)
{
    assert(handle == 1 && handle_open);
    handle_open = false;
    ++closes;
}

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t size)
{
    assert(length == 1 && size <= sizeof(((struct test_queue *)0)->data));
    if (++queue_calls == queue_fail_at) return NULL;
    struct test_queue *queue = calloc(1, sizeof(*queue));
    assert(queue);
    queue->size = size;
    return queue;
}
void vQueueDelete(QueueHandle_t queue) { assert(queue); ++queue_deletes; free(queue); }
BaseType_t xQueueOverwrite(QueueHandle_t queue, const void *data)
{
    assert(queue && data);
    if (reject_queue) return pdFALSE;
    memcpy(queue->data, data, queue->size);
    queue->ready = true;
    return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t queue, void *data, TickType_t wait)
{
    assert(queue && data && (wait == 0 || wait == portMAX_DELAY));
    if (skip_receive) { skip_receive = false; return pdFALSE; }
    if (!queue->ready) {
        if (wait == portMAX_DELAY) longjmp(task_end, 1);
        return pdFALSE;
    }
    memcpy(data, queue->data, queue->size);
    queue->ready = false;
    return pdTRUE;
}
BaseType_t xTaskCreate(void (*entry)(void *), const char *name, uint32_t stack,
                       void *user, UBaseType_t priority, void *handle)
{
    assert(strcmp(name, "linx_settings") == 0 && stack == 3072 && !user && priority == 3 && !handle);
    ++task_calls;
    task_entry = entry;
    return status("task") == ESP_OK ? pdPASS : pdFALSE;
}

static void stop(void)
{
    if (commands) vQueueDelete(commands);
    if (results) vQueueDelete(results);
    commands = results = NULL;
    task_entry = NULL;
    assert(!handle_open);
}
static void reset(void)
{
    stop();
    persisted_namespace = persisted_value = false;
    flash_value = 0;
    failure = NULL;
    queue_calls = queue_fail_at = queue_deletes = task_calls = commits = closes = 0;
    reject_queue = skip_receive = false;
}
static linx_input_settings_event_t save(linx_back_mode_t mode)
{
    linx_input_settings_event_t event;
    unsigned before = commits;
    assert(linx_input_settings_request(mode, NULL));
    assert(commits == before); /* UI 请求绝不执行慢存储。 */
    assert(!linx_input_settings_poll(&event));
    assert(task_entry);
    if (setjmp(task_end) == 0) task_entry(NULL);
    assert(linx_input_settings_poll(&event) && event.mode == mode);
    assert(!linx_input_settings_poll(&event) && !handle_open);
    return event;
}

static void test_defaults_and_failures(void)
{
    reset();
    linx_back_mode_t mode = LINX_BACK_DOUBLE_CLICK;
    linx_input_settings_event_t event;
    assert(!linx_input_settings_init(NULL));
    assert(!linx_input_settings_request(LINX_BACK_LONG_PRESS, NULL));
    assert(!linx_input_settings_poll(NULL) && !linx_input_settings_poll(&event));
    assert(linx_input_settings_init(&mode) && mode == LINX_BACK_LONG_PRESS);
    assert(!linx_input_settings_request((linx_back_mode_t)-1, NULL));
    assert(!linx_input_settings_request((linx_back_mode_t)2, NULL));
    reject_queue = true;
    assert(!linx_input_settings_request(LINX_BACK_LONG_PRESS, NULL));
    reject_queue = false;
    assert(linx_input_settings_init(&mode) && queue_calls == 2 && task_calls == 1);

    for (unsigned invalid = 0; invalid < 2; ++invalid) {
        reset(); persisted_namespace = true; persisted_value = invalid != 0; flash_value = 255;
        assert(linx_input_settings_init(&mode) && mode == LINX_BACK_LONG_PRESS);
    }
    const char *errors[] = {"init", "read_open", "get", "task"};
    for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i) {
        reset(); persisted_namespace = true; failure = errors[i];
        assert(!linx_input_settings_init(&mode) && mode == LINX_BACK_LONG_PRESS);
        assert(!commands && !results && !handle_open);
        failure = NULL;
        assert(linx_input_settings_init(&mode));
    }
    for (unsigned failed_queue = 1; failed_queue <= 2; ++failed_queue) {
        reset(); queue_fail_at = failed_queue;
        assert(!linx_input_settings_init(&mode) && !commands && !results && queue_deletes == 1);
        queue_fail_at = 0;
        assert(linx_input_settings_init(&mode));
    }
}

static void test_persistence_and_write_failures(void)
{
    reset();
    linx_back_mode_t mode;
    assert(linx_input_settings_init(&mode));
    skip_receive = true;
    assert(save(LINX_BACK_DOUBLE_CLICK).saved && commits == 1);
    stop(); /* 模拟销毁运行对象后重新启动，不读同实例 RAM 缓存。 */
    assert(linx_input_settings_init(&mode) && mode == LINX_BACK_DOUBLE_CLICK);
    const char *errors[] = {"write_open", "set", "set_after_write", "commit"};
    for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i) {
        failure = errors[i];
        assert(!save(LINX_BACK_LONG_PRESS).saved);
        failure = NULL;
        stop();
        assert(linx_input_settings_init(&mode));
        /* 写入前失败保留旧值；写入后报错和提交失败的持久值不保证回滚。 */
        assert(mode == (i < 2 ? LINX_BACK_DOUBLE_CLICK : LINX_BACK_LONG_PRESS));
        assert(save(LINX_BACK_DOUBLE_CLICK).saved);
    }
    assert(save(LINX_BACK_LONG_PRESS).saved);
    stop();
    assert(linx_input_settings_init(&mode) && mode == LINX_BACK_LONG_PRESS);
    assert(closes >= 8);
}

int main(void)
{
    test_defaults_and_failures();
    test_persistence_and_write_failures();
    reset();
    puts("返回方式存储：默认长按、异步写入、重启读回、真实写入错误及资源清理通过");
    return 0;
}
