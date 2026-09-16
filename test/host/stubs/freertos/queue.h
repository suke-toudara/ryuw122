#pragma once
#include "FreeRTOS.h"

typedef void *QueueHandle_t;

QueueHandle_t xQueueCreate(uint32_t length, uint32_t item_size);
void vQueueDelete(QueueHandle_t queue);
BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t ticks);
BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t ticks);
BaseType_t xQueueReset(QueueHandle_t queue);
