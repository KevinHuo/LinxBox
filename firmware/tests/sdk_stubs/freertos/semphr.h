#ifndef LINX_TEST_FREERTOS_SEMPHR_H
#define LINX_TEST_FREERTOS_SEMPHR_H
#include "sdk_stubs.h"
typedef struct test_semaphore *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateMutex(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t wait);
BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore);
void vSemaphoreDelete(SemaphoreHandle_t semaphore);
#endif
