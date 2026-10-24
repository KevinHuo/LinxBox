#pragma once

#include <stdint.h>

enum { ESP_SLEEP_WAKEUP_GPIO = 7 };
uint32_t esp_sleep_get_wakeup_causes(void);
