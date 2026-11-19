// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dirk Tostmann
#include <stdio.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "esp_task_wdt.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "app.h"
#include "board.h"

static const char *TAG = "main";
static bool s_wdt;   // the task watchdog runs

void app_heap(const char *phase)
{
    ESP_LOGI(TAG, "heap[%s] free=%u min=%u largest=%u t=%llus", phase,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned long long)(esp_timer_get_time() / 1000000));
}

// The device server waits for Berry under its lock, and the binding, the link and the gateway's task wait for the
// device server: a task that stopped would stop the gateway for good while the link layer goes on acknowledging its
// frames. The idle task is not watched: Berry at full load starves it for seconds, and a busy gateway must not restart
// for that, least of all a TULX32 that runs from the bus.
void wdt_start(void)
{
    const esp_task_wdt_config_t c = {.timeout_ms = WDT_TIMEOUT_S * 1000, .idle_core_mask = 0, .trigger_panic = true};
    esp_err_t e = esp_task_wdt_init(&c);
    s_wdt = e == ESP_OK;
    if (s_wdt) {
        ESP_LOGI(TAG, "task watchdog: %d s, then a panic with coredump and a restart; the idle task is not watched",
                 WDT_TIMEOUT_S);
    } else {
        ESP_LOGE(TAG, "task watchdog not started: %s", esp_err_to_name(e));
    }
}

void wdt_watch(void)
{
    esp_err_t e = s_wdt ? esp_task_wdt_add(NULL) : ESP_ERR_INVALID_STATE;
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "task watchdog: task %s not watched (%s)", pcTaskGetName(NULL), esp_err_to_name(e));
    }
}

// Called from shared code as well (bvm.c, after each request): only a watched task feeds. esp_task_wdt_status() looks
// the task up without a log line, esp_task_wdt_reset() would log one for a task that is not watched.
void wdt_feed(void)
{
    if (s_wdt && esp_task_wdt_status(NULL) == ESP_OK) {
        esp_task_wdt_reset();
    }
}

static void tick(void *arg)
{
    (void)arg;
    app_heap("tick");
}

void app_main(void)
{
    const esp_app_desc_t *d = esp_app_get_description();
    ESP_LOGI(TAG, "zigbee2knx %s, IDF %s, %s", d->version, d->idf_ver, BOARD_NAME);
    wdt_start();   // before the tasks: each watched one subscribes itself when it starts
    app_heap("boot");
    esp_err_t e = nvs_flash_init();
    if ((e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) && BOARD_NVS_ERASABLE) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        e = nvs_flash_init();
    }
    if (e != ESP_OK) {   // TULX32: nvs belongs to the recovery system as well, it is never erased here
        ESP_LOGE(TAG, "nvs: %s; settings are not kept", esp_err_to_name(e));
    }
    knx_link_start();
    vTaskDelay(pdMS_TO_TICKS(300));
    app_heap("knx");
    if (bvm_start() != ESP_OK) {
        ESP_LOGE(TAG, "Berry did not load, continuing without it");
    }
    app_heap("berry");
    pair_init();    // before Zigbee: an announce or a leave of the first seconds waits in the queue
    zb_start();
    vTaskDelay(pdMS_TO_TICKS(3000));
    app_heap("zigbee");
    zlink_start();
    vdev_start();   // after Zigbee: restoring a device links its instance and reads the lamp
    pair_start();   // after the device server: binding a Zigbee device when ETS asks for the programming button
    gw_start();     // after the binding: the gateway's parameters go there; button S1 and LED
    app_heap("vdev");
    console_start();
    const esp_timer_create_args_t ta = {.callback = tick, .name = "heaptick"};
    esp_timer_handle_t t;
    ESP_ERROR_CHECK(esp_timer_create(&ta, &t));
    ESP_ERROR_CHECK(esp_timer_start_periodic(t, 60ULL * 1000000ULL));
}
