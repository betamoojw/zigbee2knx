#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
typedef uint32_t nvs_handle_t;
#define NVS_READWRITE 1
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len);
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *val, size_t len);
esp_err_t nvs_commit(nvs_handle_t h);
esp_err_t nvs_get_u8(nvs_handle_t h, const char *key, uint8_t *v);
esp_err_t nvs_get_u16(nvs_handle_t h, const char *key, uint16_t *v);
esp_err_t nvs_set_u8(nvs_handle_t h, const char *key, uint8_t v);
esp_err_t nvs_set_u16(nvs_handle_t h, const char *key, uint16_t v);
esp_err_t nvs_erase_key(nvs_handle_t h, const char *key);
esp_err_t nvs_open_from_partition(const char *part, const char *ns, int mode, nvs_handle_t *h);

typedef struct {
    size_t used_entries, free_entries, available_entries, total_entries, namespace_count;
} nvs_stats_t;
esp_err_t nvs_get_stats(const char *part, nvs_stats_t *st);

typedef enum { NVS_TYPE_BLOB = 0x42, NVS_TYPE_ANY = 0xff } nvs_type_t;
typedef struct {
    char namespace_name[16];
    char key[16];
    nvs_type_t type;
} nvs_entry_info_t;
typedef struct nvs_opaque_iterator_t *nvs_iterator_t;
esp_err_t nvs_entry_find(const char *part, const char *ns, nvs_type_t type, nvs_iterator_t *it);
esp_err_t nvs_entry_next(nvs_iterator_t *it);
esp_err_t nvs_entry_info(const nvs_iterator_t it, nvs_entry_info_t *info);
void nvs_release_iterator(nvs_iterator_t it);
