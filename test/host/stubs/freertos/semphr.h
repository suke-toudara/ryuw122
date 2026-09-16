#pragma once
#include "queue.h"

typedef void *SemaphoreHandle_t;

SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void);
SemaphoreHandle_t xSemaphoreCreateBinary(void);
void vSemaphoreDelete(SemaphoreHandle_t sem);
BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t sem, TickType_t ticks);
BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t sem);
BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks);
BaseType_t xSemaphoreGive(SemaphoreHandle_t sem);
