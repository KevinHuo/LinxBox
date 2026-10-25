#include "linx_input_settings.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

static QueueHandle_t commands;
static QueueHandle_t results;

static bool load_mode(linx_back_mode_t *mode)
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open("linx_ui", NVS_READONLY, &handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) return true;
    if (error != ESP_OK) return false;
    uint8_t value = 0;
    error = nvs_get_u8(handle, "back_mode", &value);
    nvs_close(handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) return true;
    if (error != ESP_OK) return false;
    if (value <= LINX_BACK_DOUBLE_CLICK) *mode = (linx_back_mode_t)value;
    return true;
}

static bool save_mode(linx_back_mode_t mode)
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open("linx_ui", NVS_READWRITE, &handle);
    if (error != ESP_OK) return false;
    error = nvs_set_u8(handle, "back_mode", (uint8_t)mode);
    if (error == ESP_OK) error = nvs_commit(handle);
    nvs_close(handle);
    return error == ESP_OK;
}

static void worker(void *user)
{
    (void)user;
    linx_back_mode_t mode;
    for (;;) {
        if (xQueueReceive(commands, &mode, portMAX_DELAY) != pdTRUE) continue;
        const linx_input_settings_event_t event = {.mode = mode, .saved = save_mode(mode)};
        xQueueOverwrite(results, &event);
    }
}

bool linx_input_settings_init(linx_back_mode_t *loaded_mode)
{
    if (!loaded_mode) return false;
    *loaded_mode = LINX_BACK_LONG_PRESS;
    if (nvs_flash_init() != ESP_OK || !load_mode(loaded_mode)) return false;
    if (commands) return true;
    QueueHandle_t command_queue = xQueueCreate(1, sizeof(linx_back_mode_t));
    QueueHandle_t result_queue = xQueueCreate(1, sizeof(linx_input_settings_event_t));
    if (!command_queue || !result_queue) {
        if (command_queue) vQueueDelete(command_queue);
        if (result_queue) vQueueDelete(result_queue);
        return false;
    }
    commands = command_queue;
    results = result_queue;
    if (xTaskCreate(worker, "linx_settings", 3072, NULL, 3, NULL) != pdPASS) {
        vQueueDelete(commands);
        vQueueDelete(results);
        commands = NULL;
        results = NULL;
        return false;
    }
    return true;
}

bool linx_input_settings_request(linx_back_mode_t mode, void *user)
{
    (void)user;
    return (unsigned)mode <= LINX_BACK_DOUBLE_CLICK && commands &&
           xQueueOverwrite(commands, &mode) == pdTRUE;
}

bool linx_input_settings_poll(linx_input_settings_event_t *event)
{
    return event && results && xQueueReceive(results, event, 0) == pdTRUE;
}
