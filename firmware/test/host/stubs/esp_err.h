#pragma once
#include <stdint.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_TIMEOUT 0x107
#define ESP_ERR_NVS_NO_FREE_PAGES 0x110d
#define ESP_ERR_NVS_NEW_VERSION_FOUND 0x1110
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define ESP_ERR_NVS_NOT_ENOUGH_SPACE 0x1105
static inline const char *esp_err_to_name(esp_err_t e) { return e == ESP_OK ? "ESP_OK" : "ESP_ERR"; }
#define ESP_ERROR_CHECK(x) do { if ((x) != ESP_OK) { __builtin_trap(); } } while (0)
