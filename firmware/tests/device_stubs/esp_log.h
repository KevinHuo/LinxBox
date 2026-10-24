#pragma once

void device_test_log(char level, const char *tag, const char *format, ...)
    __attribute__((format(printf, 3, 4)));

#define ESP_LOGI(tag, ...) device_test_log('I', tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) device_test_log('W', tag, __VA_ARGS__)
#define ESP_LOGE(tag, ...) device_test_log('E', tag, __VA_ARGS__)
