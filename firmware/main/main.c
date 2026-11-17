// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dirk Tostmann
#include <stdio.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "app.h"
#include "board.h"

static const char *TAG = "main";

void app_heap(const char *phase)
{
    ESP_LOGI(TAG, "heap[%s] free=%u min=%u largest=%u t=%llus", phase,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned long long)(esp_timer_get_time() / 1000000));
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
