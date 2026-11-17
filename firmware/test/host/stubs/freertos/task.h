#pragma once
#include "FreeRTOS.h"
typedef void *TaskHandle_t;
BaseType_t xTaskCreate(void (*fn)(void *), const char *name, uint32_t stack, void *arg, UBaseType_t prio, TaskHandle_t *h);
void vTaskDelay(TickType_t t);
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t h);
// task notifications as a counting semaphore
BaseType_t xTaskNotifyGive(TaskHandle_t h);
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait);
void vTaskList(char *buf);
