#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#define UART_NUM_1 1
#define UART_DATA_8_BITS 3
#define UART_PARITY_EVEN 2
#define UART_STOP_BITS_1 1
#define UART_HW_FLOWCTRL_DISABLE 0
#define UART_SCLK_DEFAULT 0
#define UART_PIN_NO_CHANGE -1
typedef struct { int baud_rate, data_bits, parity, stop_bits, flow_ctrl, source_clk; } uart_config_t;
esp_err_t uart_driver_install(int port, int rx, int tx, int qs, void *q, int flags);
esp_err_t uart_param_config(int port, const uart_config_t *c);
esp_err_t uart_set_pin(int port, int tx, int rx, int rts, int cts);
esp_err_t uart_set_rx_full_threshold(int port, int n);
esp_err_t uart_set_rx_timeout(int port, int n);
int uart_write_bytes(int port, const void *src, size_t n);
int uart_read_bytes(int port, void *buf, uint32_t n, TickType_t ticks);
esp_err_t uart_get_buffered_data_len(int port, size_t *n);
