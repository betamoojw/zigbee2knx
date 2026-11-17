#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
int64_t esp_timer_get_time(void);
// the timers zlink.c uses; zlink_test.c drives them on its virtual clock (other tests do not use them)
struct stub_timer {
    void (*cb)(void *);
    void *arg;
    const char *name;
    bool armed;
    int64_t due;
    uint64_t period;
};
typedef struct stub_timer *esp_timer_handle_t;
typedef struct {
    void (*callback)(void *);
    void *arg;
    const char *name;
} esp_timer_create_args_t;
esp_err_t esp_timer_create(const esp_timer_create_args_t *a, esp_timer_handle_t *h);
esp_err_t esp_timer_start_once(esp_timer_handle_t h, uint64_t us);
esp_err_t esp_timer_start_periodic(esp_timer_handle_t h, uint64_t us);
esp_err_t esp_timer_stop(esp_timer_handle_t h);
