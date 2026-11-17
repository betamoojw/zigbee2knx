#pragma once
#include "FreeRTOS.h"
typedef struct stub_queue *QueueHandle_t;
QueueHandle_t xQueueCreate(UBaseType_t n, UBaseType_t size);
BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t t);
BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t t);
UBaseType_t uxQueueSpacesAvailable(QueueHandle_t q);
