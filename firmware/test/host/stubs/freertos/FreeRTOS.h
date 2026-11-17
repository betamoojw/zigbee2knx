#pragma once
#include <stdint.h>
#include <stddef.h>
typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef uint8_t StackType_t;
#define pdTRUE 1
#define pdFALSE 0
#define portMAX_DELAY 0xffffffffu
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define portTICK_PERIOD_MS 1
