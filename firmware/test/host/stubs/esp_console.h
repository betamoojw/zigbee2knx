#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
// what console.c uses of esp_console; the host tests call its command functions directly
typedef int (*esp_console_cmd_func_t)(int argc, char **argv);
typedef struct {
    const char *command, *help, *hint;
    esp_console_cmd_func_t func;
    void *argtable;
} esp_console_cmd_t;
typedef struct esp_console_repl_s esp_console_repl_t;
typedef struct {
    size_t max_history_len;
    const char *history_save_path;
    uint32_t task_stack_size, task_priority;
    const char *prompt;
    size_t max_cmdline_length;
} esp_console_repl_config_t;
#define ESP_CONSOLE_REPL_CONFIG_DEFAULT() {0}
typedef struct { int unused; } esp_console_dev_usb_serial_jtag_config_t;
#define ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT() {0}
typedef struct { int unused; } esp_console_dev_uart_config_t;
#define ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT() {0}
esp_err_t esp_console_cmd_register(const esp_console_cmd_t *cmd);
esp_err_t esp_console_new_repl_usb_serial_jtag(const esp_console_dev_usb_serial_jtag_config_t *dev,
                                               const esp_console_repl_config_t *cfg, esp_console_repl_t **repl);
esp_err_t esp_console_new_repl_uart(const esp_console_dev_uart_config_t *dev, const esp_console_repl_config_t *cfg,
                                    esp_console_repl_t **repl);
esp_err_t esp_console_register_help_command(void);
esp_err_t esp_console_start_repl(esp_console_repl_t *repl);
