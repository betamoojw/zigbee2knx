#pragma once
#include <stdarg.h>
#include <stdio.h>
// every log line is really formatted, so a bad argument (string without end, wrong length) shows up under ASan
void stub_log(char lvl, const char *tag, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
#define ESP_LOGI(tag, fmt, ...) stub_log('I', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) stub_log('W', tag, fmt, ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...) stub_log('E', tag, fmt, ##__VA_ARGS__)
