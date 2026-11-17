#pragma once
#include "FreeRTOS.h"
typedef void *TaskHandle_t;
BaseType_t xTaskCreate(void (*fn)(void *), const char *name, uint32_t stack, void *arg, UBaseType_t prio, TaskHandle_t *h);
void vTaskDelay(TickType_t t);
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t h);
