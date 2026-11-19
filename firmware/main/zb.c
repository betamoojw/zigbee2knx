// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dirk Tostmann
// Zigbee coordinator on the native esp-zigbee-lib 2.x API (esp_zigbee.h + ezbee/*), no 1.x compat layer.
// Forms a network on ZB_CHANNEL; joining only on request (console "join"), so no foreign device joins by chance.
// Endpoint ZB_EP is a gateway with Basic, On/Off, Level and Color Control client clusters: the console switches, dims
// and reads devices from there, and every incoming ZCL frame is logged.
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_zigbee.h"
#include "ezbee/zha.h"
#include "app.h"
#include "board.h"
#include "zbstart.h"

static const char *TAG = "zb";
// Openings of the network this firmware asked for and has not seen confirmed yet, and the duration it asked for.
// A status signal that matches neither is somebody else's doing.
static atomic_int s_own_open;
static atomic_uint s_own_secs;

#define ZB_CHANNEL 25   // the only channel of the primary channel set
#define ZB_STORAGE BOARD_ZB_PART   // "zb_storage" on the TUL32; on the TULX32 the shared "nvs" (board.h)
#define ZB_EP      1    // our endpoint: commands leave from here, responses and reports arrive here

static volatile bool s_up;
static volatile uint32_t s_annce, s_auth, s_leave, s_update;
static esp_timer_handle_t s_retry;
static TaskHandle_t s_task;
// A start without the network the stack had (zbstart.h): with Zigbee devices paired no new one is formed, s_lost tells
// the gateway's fault object. A new one only on request: "zbform yes" (s_form_now) or "zbreset yes", which sets the
// mark in RTC memory before its restart (survives a software restart, not a power cut).
#define ZB_FORM_MAGIC 0x5a42464fu
static volatile bool s_lost, s_form_now;
static esp_timer_handle_t s_decide;
RTC_NOINIT_ATTR static uint32_t s_form_asked;

static void form_cb(void *ctx)
{
    (void)ctx;
    ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_FORMATION);
}

static void retry_fire(void *arg)
{
    (void)arg;
    esp_zigbee_task_queue_post(form_cb, NULL);
}

static void decide_cb(void *ctx)   // in the Zigbee task: the stack started without a network (zbstart.h)
{
    (void)ctx;
    int bound = 0, waiting = 0;
    bool loaded = vdev_loaded();
    if (loaded) {
        vdev_counts(&bound, &waiting);
    }
    bool asked = s_form_now || s_form_asked == ZB_FORM_MAGIC;
    switch (zbstart_decide(false, loaded, bound + waiting, asked)) {
    case ZBSTART_WAIT:
        esp_timer_start_once(s_decide, 500000);
        return;
    case ZBSTART_REFUSE:
        if (!s_lost) {
            s_lost = true;
            ESP_LOGE(TAG, "the Zigbee network is gone (the stack starts as new) while %d Zigbee devices are paired: "
                     "no new network is formed, it would take every device away. \"zbform yes\" at the console "
                     "forms one; each device then has to be reset and join anew", bound + waiting);
            gw_note(GW_NOTE_NET, 0xFFFF, 0, "Zigbee network gone, none formed");
        }
        return;
    default:
        s_lost = false;
        s_form_now = false;
        s_form_asked = 0;
        ESP_LOGI(TAG, "forming a network%s", asked ? " (asked for)" : "");
        ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_FORMATION);
        return;
    }
}

static void decide_fire(void *arg)
{
    (void)arg;
    esp_zigbee_task_queue_post(decide_cb, NULL);
}

// The TX power cap of the board (board.h: BOARD_ZB_TX_DBM; the TULX32 runs from the bus). Set when the stack starts,
// and again once the network is formed or restored, because the header does not say whether forming or restoring keeps
// it. Runs in the stack's task (signal handler): no lock to take.
static void tx_cap(const char *when)
{
#ifdef BOARD_ZB_TX_DBM
    int8_t now = 0;
    ezb_nwk_set_tx_power(BOARD_ZB_TX_DBM);
    ezb_nwk_get_tx_power(&now);
    ESP_LOGI(TAG, "TX power capped at %d dBm (%s), reads back %d dBm", BOARD_ZB_TX_DBM, when, now);
#else
    (void)when;
#endif
}

static void eui_str(const ezb_extaddr_t *a, char out[17])
{
    snprintf(out, 17, "%02x%02x%02x%02x%02x%02x%02x%02x", a->u8[7], a->u8[6], a->u8[5], a->u8[4], a->u8[3],
             a->u8[2], a->u8[1], a->u8[0]);
}

static void addr_str(const ezb_address_t *a, char out[20])
{
    if (a->addr_mode == EZB_ADDR_MODE_EXT) {
        eui_str(&a->u.extended_addr, out);
    } else {
        snprintf(out, 20, "0x%04x", a->u.short_addr);
    }
}

static void hdr_src(const ezb_zcl_cmd_hdr_t *h, char out[20])
{
    if (h) {
        addr_str(&h->src_addr, out);
    } else {
        strcpy(out, "?");
    }
}

// ZCL attribute value as text; strings arrive with their length byte in front
static void attr_str(uint8_t type, const void *val, char *out, size_t n)
{
    const uint8_t *p = val;
    if (!p) {
        snprintf(out, n, "(no value)");
        return;
    }
    switch (type) {
    case 0x10:   // boolean
    case 0x18:   // bitmap8
    case 0x20:   // uint8
    case 0x30:   // enum8
        snprintf(out, n, "%u (type 0x%02x)", p[0], type);
        break;
    case 0x19:   // bitmap16
    case 0x21:   // uint16
    case 0x31:   // enum16
        snprintf(out, n, "%u (type 0x%02x)", (unsigned)(p[0] | p[1] << 8), type);
        break;
    case 0x23:   // uint32
        snprintf(out, n, "%lu", (unsigned long)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24));
        break;
    case 0x28:   // int8
        snprintf(out, n, "%d", (int8_t)p[0]);
        break;
    case 0x29:   // int16
        snprintf(out, n, "%d", (int16_t)(p[0] | p[1] << 8));
        break;
    case 0x41:   // octet string
    case 0x42: { // character string
        int len = p[0] == 0xff ? 0 : p[0];
        snprintf(out, n, "\"%.*s\"", len, (const char *)p + 1);
        break;
    }
    default:
        snprintf(out, n, "type 0x%02x", type);
        break;
    }
}

// every ZCL frame that reaches our endpoint, before the stack handles it (false = let the stack go on)
static bool on_zcl_raw(const ezb_zcl_raw_frame_t *f)
{
    const ezb_zcl_cmd_hdr_t *h = f->header;
    char src[20], hex[3 * 24 + 1] = "";
    hdr_src(h, src);
    int n = f->payload_length < 24 ? f->payload_length : 24;
    for (int i = 0; i < n; i++) {
        snprintf(hex + 3 * i, 4, "%02x ", f->payload[i]);
    }
    if (h) {
        if (h->src_addr.addr_mode != EZB_ADDR_MODE_EXT) {
            pair_zb_heard(h->src_addr.u.short_addr);   // the device is there and answers
            zlink_heard(h->src_addr.u.short_addr);     // ... also for its availability (avail.h)
        }
        ESP_LOGI(TAG, "zcl in: %s ep %u cluster 0x%04x cmd 0x%02x fc 0x%02x tsn %u rssi %d, %u B: %s%s", src, h->src_ep,
                 h->cluster_id, h->cmd_id, h->fc, h->tsn, h->rssi, f->payload_length, hex,
                 f->payload_length > n ? "..." : "");
        // A command a device sends to us (cluster-specific, client to server: a remote's On, Off, Move, ...): its
        // template decides what it means on KNX. Taken here, the stack does not answer it with "unsupported".
        if ((h->fc & 0x03) == 0x01 && !(h->fc & 0x08) && h->src_addr.addr_mode == EZB_ADDR_MODE_SHORT &&
            zlink_cmd(h->src_addr.u.short_addr, h->cluster_id, h->cmd_id, f->payload, f->payload_length)) {
            return true;
        }
    }
    return false;
}

// a value from a device goes to its linked Berry instance, if it has one (zlink.c; never blocks this task)
static void to_link(const ezb_zcl_cmd_hdr_t *h, uint16_t attr, uint8_t type, const void *val)
{
    if (h && h->src_addr.addr_mode == EZB_ADDR_MODE_SHORT) {
        zlink_attr_ep(h->src_addr.u.short_addr, h->src_ep, h->cluster_id, attr, type, val);
    }
}

static void on_zcl_action(ezb_zcl_core_action_callback_id_t id, void *msg)
{
    char src[20], val[72];
    switch (id) {
    case EZB_ZCL_CORE_READ_ATTR_RSP_CB_ID: {
        ezb_zcl_cmd_read_attr_rsp_message_t *m = msg;
        hdr_src(m->in.header, src);
        for (ezb_zcl_read_attr_rsp_variable_t *v = m->in.variables; v; v = v->next) {
            if (v->status == 0) {
                attr_str(v->attr_type, v->attr_value, val, sizeof val);
                to_link(m->in.header, v->attr_id, v->attr_type, v->attr_value);
            } else {
                snprintf(val, sizeof val, "status 0x%02x", v->status);
            }
            ESP_LOGI(TAG, "read %s cluster 0x%04x attr 0x%04x: %s", src,
                     m->in.header ? m->in.header->cluster_id : 0, v->attr_id, val);
        }
        m->out.result = EZB_ZCL_STATUS_SUCCESS;
        break;
    }
    case EZB_ZCL_CORE_REPORT_ATTR_CB_ID: {
        ezb_zcl_cmd_report_attr_message_t *m = msg;
        hdr_src(m->in.header, src);
        for (ezb_zcl_report_attr_variable_t *v = m->in.variables; v; v = v->next) {
            attr_str(v->attr_type, v->attr_value, val, sizeof val);
            to_link(m->in.header, v->attr_id, v->attr_type, v->attr_value);
            ESP_LOGI(TAG, "report %s cluster 0x%04x attr 0x%04x: %s", src,
                     m->in.header ? m->in.header->cluster_id : 0, v->attr_id, val);
        }
        m->out.result = EZB_ZCL_STATUS_SUCCESS;
        break;
    }
    case EZB_ZCL_CORE_WRITE_ATTR_RSP_CB_ID: {
        ezb_zcl_cmd_write_attr_rsp_message_t *m = msg;
        hdr_src(m->in.header, src);
        for (ezb_zcl_write_attr_rsp_variable_t *v = m->in.variables; v; v = v->next) {
            ESP_LOGI(TAG, "write rsp %s cluster 0x%04x attr 0x%04x: status 0x%02x", src,
                     m->in.header ? m->in.header->cluster_id : 0, v->attr_id, (unsigned)v->status);
        }
        m->out.result = EZB_ZCL_STATUS_SUCCESS;
        break;
    }
    case EZB_ZCL_CORE_DEFAULT_RSP_CB_ID: {
        ezb_zcl_cmd_default_rsp_message_t *m = msg;
        hdr_src(m->in.header, src);
        ESP_LOGI(TAG, "default rsp %s cluster 0x%04x to cmd 0x%02x: status 0x%02x", src,
                 m->in.header ? m->in.header->cluster_id : 0, m->in.rsp_to_cmd, m->in.status_code);
        m->out.result = EZB_ZCL_STATUS_SUCCESS;
        break;
    }
    default:
        ESP_LOGI(TAG, "zcl action %lu", (unsigned long)id);
        break;
    }
}

// APS data confirm for our own requests: status 0 = delivered to the next hop (and acked, if APS acks are on)
static void on_cnf(ezb_af_user_cnf_t *c, void *ctx)
{
    (void)ctx;
    char dst[20];
    addr_str(&c->dst_addr, dst);
    ESP_LOGI(TAG, "sent to %s ep %u cluster 0x%04x tsn %u: status 0x%02x", dst, c->dst_ep, c->cluster_id, c->tsn,
             c->status);
    if (c->dst_addr.addr_mode == EZB_ADDR_MODE_SHORT) {
        zlink_sent(c->dst_addr.u.short_addr, c->status);   // a failed unicast counts against its availability
    }
}

static void ep_register(void)
{
    ezb_zha_custom_gateway_config_t gw = EZB_ZHA_CUSTOM_GATEWAY_CONFIG();
    ezb_af_ep_desc_t ep = ezb_zha_create_custom_gateway(ZB_EP, &gw);
    if (!ep) {
        ESP_LOGE(TAG, "gateway endpoint: create failed");
        return;
    }
    const struct {
        const char *name;
        ezb_zcl_cluster_desc_t desc;
    } cl[] = {
        {"basic", ezb_zcl_basic_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_CLIENT)},
        {"on_off", ezb_zcl_on_off_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_CLIENT)},
        {"level", ezb_zcl_level_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_CLIENT)},
        {"color_control", ezb_zcl_color_control_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_CLIENT)},
        {"identify", ezb_zcl_identify_create_cluster_desc(NULL, EZB_ZCL_CLUSTER_CLIENT)},
    };
    for (size_t i = 0; i < sizeof cl / sizeof cl[0]; i++) {
        ezb_err_t e = cl[i].desc ? ezb_af_endpoint_add_cluster_desc(ep, cl[i].desc) : EZB_ERR_NO_MEM;
        if (e != EZB_ERR_NONE) {
            ESP_LOGW(TAG, "gateway endpoint: %s client not added (%d)", cl[i].name, (int)e);
        }
    }
    ezb_af_device_desc_t dev = ezb_af_create_device_desc();
    ESP_ERROR_CHECK(esp_zigbee_err_to_esp(ezb_af_device_add_endpoint_desc(dev, ep)));
    ESP_ERROR_CHECK(esp_zigbee_err_to_esp(ezb_af_device_desc_register(dev)));
    ezb_zcl_core_action_handler_register(on_zcl_action);
    ezb_zcl_raw_command_handler_register(on_zcl_raw);
}

static bool on_signal(const ezb_app_signal_t *sig)
{
    ezb_app_signal_type_t t = ezb_app_signal_get_type(sig);
    const void *p = ezb_app_signal_get_params(sig);
    char eui[17];
    switch (t) {
    case EZB_ZDO_SIGNAL_SKIP_STARTUP:
        tx_cap("stack started");
        ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_INITIALIZATION);
        return true;
    case EZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case EZB_BDB_SIGNAL_DEVICE_REBOOT: {
        const ezb_bdb_signal_simple_params_t *s = p;
        uint8_t st = s ? s->status : 0xff;
        if (st == EZB_BDB_STATUS_SUCCESS && !ezb_bdb_is_factory_new()) {
            s_up = true;
            ESP_LOGI(TAG, "network restored: channel %u, PAN 0x%04x", ezb_get_current_channel(), ezb_get_panid());
            tx_cap("network restored");
        } else {
            ESP_LOGI(TAG, "%s (status %u): no network restored", ezb_app_signal_to_string(t), st);
            decide_cb(NULL);
        }
        return true;
    }
    case EZB_BDB_SIGNAL_FORMATION: {
        const ezb_bdb_signal_simple_params_t *s = p;
        if (s && s->status == EZB_BDB_STATUS_SUCCESS) {
            s_up = true;
            s_lost = false;
            ESP_LOGI(TAG, "formed: channel %u, PAN 0x%04x", ezb_get_current_channel(), ezb_get_panid());
            tx_cap("network formed");
            app_heap("zb_formed");
        } else {
            ESP_LOGW(TAG, "formation failed (status %u), retry in 1 s", s ? s->status : 0xff);
            esp_timer_start_once(s_retry, 1000000);
        }
        return true;
    }
    case EZB_ZDO_SIGNAL_DEVICE_ANNCE: {
        const ezb_zdo_signal_device_annce_params_t *a = p;
        s_annce++;
        eui_str(&a->device_addr, eui);
        ESP_LOGI(TAG, "device announce: 0x%04x %s capability 0x%02x", a->short_addr, eui, a->capability);
        pair_zb_announce(a->short_addr, a->device_addr.u8, a->capability);
        zlink_heard(a->short_addr);
        return false;
    }
    case EZB_ZDO_SIGNAL_DEVICE_AUTHORIZED: {
        const ezb_zdo_signal_device_authorized_params_t *a = p;
        s_auth++;
        eui_str(&a->device_addr, eui);
        static const char *const auth_type[] = {"LEGACY", "TCLK", "DLK"};   // EZB_ZDO_AUTH_TYPE_* in order
        ESP_LOGI(TAG, "device authorized: 0x%04x %s type %s status %d", a->short_addr, eui,
                 (unsigned)a->type < sizeof auth_type / sizeof auth_type[0] ? auth_type[a->type] : "?",
                 (int)a->status);
        pair_zb_authorized(a->short_addr, a->status == 0);
        return false;
    }
    case EZB_ZDO_SIGNAL_DEVICE_UPDATE: {
        const ezb_zdo_signal_device_update_params_t *u = p;
        s_update++;
        eui_str(&u->device_addr, eui);
        ESP_LOGI(TAG, "device update: 0x%04x %s status %d tc_action %d parent 0x%04x", u->short_addr, eui,
                 (int)u->status, (int)u->tc_action, u->parent_short);
        pair_zb_update(u->short_addr, u->device_addr.u8, (int)u->status);
        return false;
    }
    case EZB_ZDO_SIGNAL_LEAVE_INDICATION: {
        const ezb_zdo_signal_leave_indication_params_t *l = p;
        s_leave++;
        eui_str(&l->device_addr, eui);
        ESP_LOGI(TAG, "leave: 0x%04x %s type %d", l->short_addr, eui, (int)l->leave_type);
        pair_zb_left(l->short_addr, l->device_addr.u8);
        return false;
    }
    case EZB_NWK_SIGNAL_PERMIT_JOIN_STATUS: {
        const ezb_nwk_signal_permit_join_status_params_t *j = p;
        ESP_LOGI(TAG, "permit join: %u s", j ? j->duration : 0);
        // An opening this firmware did not ask for is closed again (pair.c). Seen on 2026-11-12: 2 s after the key
        // exchange of the LED1934G3 the network was open for 180 s; who asked for it is not established.
        if (j && j->duration) {
            if (atomic_load(&s_own_open) > 0 && j->duration == atomic_load(&s_own_secs)) {
                atomic_fetch_sub(&s_own_open, 1);
            } else {
                pair_zb_opened(j->duration);
            }
        }
        return false;
    }
    default:
        ESP_LOGI(TAG, "signal %s (0x%x)", ezb_app_signal_to_string(t), (unsigned)t);
        return false;
    }
}

static void zb_task(void *arg)
{
    (void)arg;
    // The native 2.x datasets live in NVS (nvs_open_from_partition on storage_partition_name), and the library does
    // not initialise that partition itself: without this, ezb_plat_datasets_init() aborts (seen 2026-11-11).
    esp_err_t ne = nvs_flash_init_partition(ZB_STORAGE);
    if ((ne == ESP_ERR_NVS_NO_FREE_PAGES || ne == ESP_ERR_NVS_NEW_VERSION_FOUND) && BOARD_NVS_ERASABLE) {
        ESP_ERROR_CHECK(nvs_flash_erase_partition(ZB_STORAGE));
        ne = nvs_flash_init_partition(ZB_STORAGE);
    }
    if (ne != ESP_OK) {   // on the TULX32 the partition is shared with the recovery system: never erased here
        ESP_LOGE(TAG, "storage partition %s: %s; Zigbee does not start", ZB_STORAGE, esp_err_to_name(ne));
        vTaskDelete(NULL);
    }
    esp_zigbee_config_t cfg = {0};
    cfg.device_config.device_type = EZB_NWK_DEVICE_TYPE_COORDINATOR;
    cfg.device_config.install_code_policy = false;
    cfg.device_config.zczr_config.max_children = 32;
    cfg.platform_config.storage_partition_name = ZB_STORAGE;
    cfg.platform_config.radio_config.radio_mode = ESP_ZIGBEE_RADIO_MODE_NATIVE;
    ESP_ERROR_CHECK(esp_zigbee_init(&cfg));
    app_heap("zb_init");
    // From esp-zigbee-lib 2.1.0 the trust center removes, about 15 s after it joined, a device that does not exchange a
    // TC link key (devices before Zigbee 3.0), unless told otherwise; 2.0.x kept them. Keep them: devices from before
    // Zigbee 3.0 join this way. Rejoins with the well-known key stay at the library's default (not allowed).
    ezb_err_t te = ezb_secur_set_tclk_exchange_required(false);
    if (te == EZB_ERR_NONE) {
        ESP_LOGI(TAG, "trust center: joiners without a TC link key exchange stay; rejoin with the well-known key %s",
                 ezb_secur_tcpol_get_allow_rejoins_with_well_known_key() ? "allowed" : "not allowed");
    } else {
        ESP_LOGW(TAG, "trust center: keeping joiners without a TC link key exchange refused (%d): they are removed",
                 (int)te);
    }
    // sized for 48 devices; 0 keeps the library default
    ezb_mem_config_t mem = {
        .buffer_pool_size = 0,
        .address_table_size = 64,
        .neighbor_table_size = 32,
        .route_table_size = 32,
        .route_discovery_table_size = 16,
        .route_record_table_size = 32,
        .aps_key_pair_set_size = 64,
        .aps_bind_table_src_size = 16,
        .aps_bind_table_dst_size = 32,
    };
    ezb_err_t me = ezb_config_memory(&mem);
    ESP_LOGI(TAG, "ezb_config_memory: %d", (int)me);
    app_heap("zb_mem");
    ep_register();
    app_heap("zb_ep");
    ESP_ERROR_CHECK(esp_zigbee_err_to_esp(ezb_app_signal_add_handler(on_signal)));
    ESP_ERROR_CHECK(esp_zigbee_err_to_esp(ezb_bdb_set_primary_channel_set(1UL << ZB_CHANNEL)));
    ESP_ERROR_CHECK(esp_zigbee_start(false));
    app_heap("zb_started");
    ESP_ERROR_CHECK(esp_zigbee_launch_mainloop());
    vTaskDelete(NULL);
}

void zb_start(void)
{
    const esp_timer_create_args_t a = {.callback = retry_fire, .name = "zbretry"};
    ESP_ERROR_CHECK(esp_timer_create(&a, &s_retry));
    const esp_timer_create_args_t b = {.callback = decide_fire, .name = "zbdecide"};
    ESP_ERROR_CHECK(esp_timer_create(&b, &s_decide));
    xTaskCreate(zb_task, "zigbee", 6144, NULL, 5, &s_task);
}

// own: this firmware wants the network open. false leaves no trace of the request, as if a device on the network had
// opened it (console test of the closing path).
esp_err_t zb_permit_join_as(uint8_t seconds, bool own)
{
    if (!s_up) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!esp_zigbee_lock_acquire(pdMS_TO_TICKS(1000))) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t r = esp_zigbee_err_to_esp(seconds ? ezb_bdb_open_network(seconds) : ezb_bdb_close_network());
    if (own && seconds && r == ESP_OK) {   // only an opening that went out is expected back
        atomic_store(&s_own_secs, seconds);
        atomic_fetch_add(&s_own_open, 1);
    } else if (!seconds) {
        atomic_store(&s_own_open, 0);   // closed: nothing of ours is under way any more
    }
    esp_zigbee_lock_release();
    return r;
}

static const char *ezb_err_str(ezb_err_t e)   // the generic codes of ezbee/error.h, NULL for any other
{
    switch (e) {
    case EZB_ERR_FAIL: return "failure";
    case EZB_ERR_NO_MEM: return "no memory";
    case EZB_ERR_INV_ARG: return "invalid argument";
    case EZB_ERR_INV_STATE: return "invalid state";
    case EZB_ERR_NOT_FOUND: return "not found";
    case EZB_ERR_NOT_SUPPORTED: return "not supported";
    case EZB_ERR_TIMEOUT: return "timeout";
    case EZB_ERR_ABORT: return "aborted";
    case EZB_ERR_BUSY: return "busy";
    default: return NULL;
    }
}

// The outcome of our Mgmt_Leave_req. A timeout only says that no response came in time: a lamp has been seen to leave
// a second after it. Whether a device left shows in its leave indication or a Device_Update "left" (pair.c).
static void leave_cb(const ezb_zdo_nwk_mgmt_leave_req_result_t *r, void *ctx)
{
    unsigned a = (unsigned)(uintptr_t)ctx;
    if (!r) {
        ESP_LOGW(TAG, "leave request to 0x%04x: no outcome reported", a);
    } else if (r->error == EZB_ERR_TIMEOUT) {
        ESP_LOGW(TAG, "leave request to 0x%04x: no response in time; it may leave all the same, its leave indication "
                 "tells", a);
    } else if (r->error != EZB_ERR_NONE || !r->rsp) {
        const char *n = ezb_err_str(r->error);
        ESP_LOGW(TAG, "leave request to 0x%04x: no response, error 0x%04x (%s)", a, (unsigned)r->error,
                 n ? n : "see ezbee/error.h");
    } else {
        ESP_LOGI(TAG, "leave request to 0x%04x: ZDP status 0x%02x (%s)", a, (unsigned)r->rsp->status,
                 r->rsp->status == 0 ? "it confirms and leaves" : "refused");
    }
}

// ---------------------------------------------------------------- the full interview (ep_pick.h, zlink.c)
static void active_ep_cb(const ezb_zdo_active_ep_req_result_t *r, void *ctx)
{
    uint16_t a = (uint16_t)(uintptr_t)ctx;
    if (!r || r->error != EZB_ERR_NONE || !r->rsp || r->rsp->status != 0) {
        zlink_disc_eps(a, NULL, 0, !r ? -1 : r->error != EZB_ERR_NONE ? (int)r->error : r->rsp ? r->rsp->status : -2);
        return;
    }
    zlink_disc_eps(a, r->rsp->active_ep_list, r->rsp->active_ep_count, 0);
}

static void simple_desc_cb(const ezb_zdo_simple_desc_req_result_t *r, void *ctx)
{
    uint32_t c = (uint32_t)(uintptr_t)ctx;
    uint16_t a = (uint16_t)(c >> 8);
    ep_desc_t d = {.ep = (uint8_t)c};
    if (!r || r->error != EZB_ERR_NONE || !r->rsp || r->rsp->status != 0) {
        zlink_disc_desc(a, &d, !r ? -1 : r->error != EZB_ERR_NONE ? (int)r->error : r->rsp ? r->rsp->status : -2);
        return;
    }
    const ezb_af_simple_desc_t *s = &r->rsp->desc;
    d.ep = s->ep_id;
    d.profile = s->app_profile_id;
    int nin = s->app_input_cluster_count, nout = s->app_output_cluster_count;
    d.nin = (uint8_t)(nin < EP_CL_MAX ? nin : EP_CL_MAX);
    d.nout = (uint8_t)(nout < EP_CL_MAX - d.nin ? nout : EP_CL_MAX - d.nin);
    for (int i = 0; i < d.nin && s->app_cluster_list; i++) {
        d.cl[i] = s->app_cluster_list[i];
    }
    for (int i = 0; i < d.nout && s->app_cluster_list; i++) {
        d.cl[d.nin + i] = s->app_cluster_list[nin + i];
    }
    ESP_LOGI(TAG, "simple descriptor 0x%04x ep %u: profile 0x%04x device 0x%04x, %d in and %d out clusters%s", a,
             s->ep_id, s->app_profile_id, s->app_device_id, nin, nout,
             nin + nout > EP_CL_MAX ? " (not all kept)" : "");
    zlink_disc_desc(a, &d, 0);
}

static void node_desc_cb(const ezb_zdo_node_desc_req_result_t *r, void *ctx)
{
    uint16_t a = (uint16_t)(uintptr_t)ctx;
    if (!r || r->error != EZB_ERR_NONE || !r->rsp || r->rsp->status != 0) {
        zlink_disc_node(a, 0, !r ? -1 : r->error != EZB_ERR_NONE ? (int)r->error : r->rsp ? r->rsp->status : -2);
        return;
    }
    zlink_disc_node(a, r->rsp->node_desc.mac_capability_flags, 0);
}

esp_err_t zb_node_desc(uint16_t short_addr)
{
    if (!s_up) {
        return ESP_ERR_INVALID_STATE;
    }
    ezb_zdo_node_desc_req_t rq = {.dst_nwk_addr = short_addr, .field = {.nwk_addr_of_interest = short_addr},
                                  .cb = node_desc_cb, .user_ctx = (void *)(uintptr_t)short_addr};
    if (!esp_zigbee_lock_acquire(pdMS_TO_TICKS(1000))) {
        return ESP_ERR_TIMEOUT;
    }
    ezb_err_t e = ezb_zdo_node_desc_req(&rq);
    esp_zigbee_lock_release();
    return esp_zigbee_err_to_esp(e);
}

esp_err_t zb_active_ep(uint16_t short_addr)
{
    if (!s_up) {
        return ESP_ERR_INVALID_STATE;
    }
    ezb_zdo_active_ep_req_t rq = {.dst_nwk_addr = short_addr, .field = {.nwk_addr_of_interest = short_addr},
                                  .cb = active_ep_cb, .user_ctx = (void *)(uintptr_t)short_addr};
    if (!esp_zigbee_lock_acquire(pdMS_TO_TICKS(1000))) {
        return ESP_ERR_TIMEOUT;
    }
    ezb_err_t e = ezb_zdo_active_ep_req(&rq);
    esp_zigbee_lock_release();
    return esp_zigbee_err_to_esp(e);
}

esp_err_t zb_simple_desc(uint16_t short_addr, uint8_t ep)
{
    if (!s_up) {
        return ESP_ERR_INVALID_STATE;
    }
    ezb_zdo_simple_desc_req_t rq = {.dst_nwk_addr = short_addr,
                                    .field = {.nwk_addr_of_interest = short_addr, .endpoint = ep},
                                    .cb = simple_desc_cb,
                                    .user_ctx = (void *)(uintptr_t)((uint32_t)short_addr << 8 | ep)};
    if (!esp_zigbee_lock_acquire(pdMS_TO_TICKS(1000))) {
        return ESP_ERR_TIMEOUT;
    }
    ezb_err_t e = ezb_zdo_simple_desc_req(&rq);
    esp_zigbee_lock_release();
    return esp_zigbee_err_to_esp(e);
}

// ZDO Mgmt_Leave to a device, without rejoin. ieee NULL sends the device address all zero: the receiver itself leaves.
esp_err_t zb_leave(uint16_t short_addr, const uint8_t *ieee)
{
    if (!s_up) {
        return ESP_ERR_INVALID_STATE;
    }
    ezb_zdo_nwk_mgmt_leave_req_t rq = {.dst_nwk_addr = short_addr, .cb = leave_cb,
                                       .user_ctx = (void *)(uintptr_t)short_addr};
    if (ieee) {
        memcpy(rq.field.device_addr.u8, ieee, 8);
    }
    if (!esp_zigbee_lock_acquire(pdMS_TO_TICKS(1000))) {
        return ESP_ERR_TIMEOUT;
    }
    ezb_err_t e = ezb_zdo_nwk_mgmt_leave_req(&rq);
    esp_zigbee_lock_release();
    return esp_zigbee_err_to_esp(e);
}

esp_err_t zb_permit_join(uint8_t seconds)
{
    return zb_permit_join_as(seconds, true);
}

bool zb_up(void)
{
    return s_up;
}

void zb_print(void)
{
    uint8_t ch = 0;
    uint16_t pan = 0, sa = 0;
    int8_t tx = 0;
    uint8_t ls = 0;
    if (s_up && esp_zigbee_lock_acquire(pdMS_TO_TICKS(1000))) {
        ch = ezb_get_current_channel();
        pan = ezb_get_panid();
        sa = ezb_get_short_address();
        ezb_nwk_get_tx_power(&tx);
        ls = ezb_nwk_get_link_status_period();
        esp_zigbee_lock_release();
    }
    printf("zigbee: %s, channel %u, PAN 0x%04x, short 0x%04x, TX power %d dBm, link status every %u s; announce %lu, "
           "authorized %lu, update %lu, leave %lu; task stack free %u B\n",
           s_up ? "up" : "not up", ch, pan, sa, tx, ls, (unsigned long)s_annce, (unsigned long)s_auth,
           (unsigned long)s_update, (unsigned long)s_leave,
           s_task ? (unsigned)uxTaskGetStackHighWaterMark(s_task) * sizeof(StackType_t) : 0);
    if (s_lost) {
        printf("zigbee: the network is gone and devices are paired: none formed (\"zbform yes\" forms one)\n");
    }
    nvs_stats_t ns;
    if (nvs_get_stats(ZB_STORAGE, &ns) == ESP_OK) {
        printf("zb_storage: %u of %u NVS entries used, %u namespaces\n", (unsigned)ns.used_entries,
               (unsigned)ns.total_entries, (unsigned)ns.namespace_count);
    }
}

bool zb_factory_reset(void)   // false: not on this board
{
    if (!BOARD_NVS_ERASABLE) {   // the library clears "all the datasets" of a partition the recovery system shares
        return false;
    }
    s_form_asked = ZB_FORM_MAGIC;   // the start after this restart forms a new network although devices are paired
    esp_zigbee_factory_reset();     // restarts; the declaration does not say noreturn
    return true;
}

bool zb_network_lost(void)
{
    return s_lost;
}

bool zb_form_new(void)   // false: no network is missing
{
    if (!s_lost) {
        return false;
    }
    s_form_now = true;
    return esp_zigbee_task_queue_post(decide_cb, NULL) == ESP_OK;
}

bool zb_tx_power(const int8_t *set, int8_t *now)   // false: the stack is not up or busy
{
    if (!s_up || !esp_zigbee_lock_acquire(pdMS_TO_TICKS(1000))) {
        return false;
    }
    if (set) {
        ezb_nwk_set_tx_power(*set);
    }
    ezb_nwk_get_tx_power(now);
    esp_zigbee_lock_release();
    return true;
}

static ezb_zcl_cluster_cmd_ctrl_t cluster_ctrl(uint16_t dst, uint8_t ep)
{
    ezb_zcl_cluster_cmd_ctrl_t c = {0};
    c.dst_addr = EZB_ADDRESS_SHORT(dst);
    c.dst_ep = ep;
    c.src_ep = ZB_EP;
    c.cnf_ctx.cb = on_cnf;
    return c;
}

esp_err_t zb_onoff(uint16_t dst, uint8_t ep, int what)
{
    if (!s_up) {
        return ESP_ERR_INVALID_STATE;
    }
    ezb_zcl_on_off_cmd_t cmd = {.cmd_ctrl = cluster_ctrl(dst, ep)};
    if (!esp_zigbee_lock_acquire(pdMS_TO_TICKS(1000))) {
        return ESP_ERR_TIMEOUT;
    }
    ezb_err_t e = what == 0   ? ezb_zcl_on_off_off_cmd_req(&cmd)
                  : what == 1 ? ezb_zcl_on_off_on_cmd_req(&cmd)
                              : ezb_zcl_on_off_toggle_cmd_req(&cmd);
    esp_zigbee_lock_release();
    return esp_zigbee_err_to_esp(e);
}

// Move to Level with On/Off: a level above 0 switches the lamp on, 0 off. Without On/Off: On/Off is left alone, and the
// command carries ExecuteIfOff (options mask and override bit 0), so that a lamp that is off takes the level too and an
// On then brings it up to it.
esp_err_t zb_level(uint16_t dst, uint8_t ep, uint8_t level, uint16_t tenths, bool with_onoff)
{
    if (!s_up) {
        return ESP_ERR_INVALID_STATE;
    }
    ezb_zcl_level_move_to_level_cmd_t cmd = {.cmd_ctrl = cluster_ctrl(dst, ep)};
    cmd.payload.level = level;
    cmd.payload.transition_time = tenths;
    if (!esp_zigbee_lock_acquire(pdMS_TO_TICKS(1000))) {
        return ESP_ERR_TIMEOUT;
    }
    if (!with_onoff) {
        cmd.payload.options_mask = 0x01;       // ExecuteIfOff
        cmd.payload.options_override = 0x01;
    }
    ezb_err_t e = with_onoff ? ezb_zcl_level_move_to_level_with_on_off_cmd_req(&cmd) : ezb_zcl_level_move_to_level_cmd_req(&cmd);
    esp_zigbee_lock_release();
    return esp_zigbee_err_to_esp(e);
}

static ezb_zcl_cmd_ctrl_t general_ctrl(uint16_t dst, uint8_t ep, uint16_t cluster)
{
    ezb_zcl_cmd_ctrl_t c = {0};
    c.dst_addr = EZB_ADDRESS_SHORT(dst);
    c.dst_ep = ep;
    c.src_ep = ZB_EP;
    c.cluster_id = cluster;
    c.manuf_code = EZB_ZCL_STD_MANUF_CODE;
    c.cnf_ctx.cb = on_cnf;
    return c;
}

esp_err_t zb_read(uint16_t dst, uint8_t ep, uint16_t cluster, const uint16_t *attrs, int n)
{
    static uint16_t ids[8];   // touched only under the stack lock; static in case the stack reads it after return
    if (!s_up) {
        return ESP_ERR_INVALID_STATE;
    }
    if (n < 1 || n > 8) {
        return ESP_ERR_INVALID_ARG;
    }
    ezb_zcl_read_attr_cmd_t cmd = {.cmd_ctrl = general_ctrl(dst, ep, cluster)};
    if (!esp_zigbee_lock_acquire(pdMS_TO_TICKS(1000))) {
        return ESP_ERR_TIMEOUT;
    }
    memcpy(ids, attrs, n * sizeof ids[0]);
    cmd.payload.attr_number = n;
    cmd.payload.attr_field = ids;
    ezb_err_t e = ezb_zcl_read_attr_cmd_req(&cmd);
    esp_zigbee_lock_release();
    return esp_zigbee_err_to_esp(e);
}

// one attribute of up to 4 octets, value little endian as ZCL carries it; manuf 0: a standard attribute
static esp_err_t write_attr(uint16_t dst, uint8_t ep, uint16_t cluster, uint16_t attr, uint8_t type, uint32_t value,
                            uint8_t size, uint16_t manuf)
{
    static uint8_t v[4];           // touched only under the stack lock
    static ezb_zcl_attribute_t a;
    if (!s_up) {
        return ESP_ERR_INVALID_STATE;
    }
    if (size < 1 || size > 4) {
        return ESP_ERR_INVALID_ARG;
    }
    ezb_zcl_write_attr_cmd_t cmd = {.cmd_ctrl = general_ctrl(dst, ep, cluster)};
    if (manuf) {
        cmd.cmd_ctrl.manuf_code = manuf;
        cmd.cmd_ctrl.fc.manuf_specific = 1;
    }
    if (!esp_zigbee_lock_acquire(pdMS_TO_TICKS(1000))) {
        return ESP_ERR_TIMEOUT;
    }
    for (int i = 0; i < 4; i++) {
        v[i] = (uint8_t)(value >> (8 * i));
    }
    a.id = attr;
    a.data.type = type;
    a.data.size = size;
    a.data.value = v;
    cmd.payload.attr_number = 1;
    cmd.payload.attr_field = &a;
    ezb_err_t e = ezb_zcl_write_attr_cmd_req(&cmd);
    esp_zigbee_lock_release();
    return esp_zigbee_err_to_esp(e);
}

esp_err_t zb_write_u8(uint16_t dst, uint8_t ep, uint16_t cluster, uint16_t attr, uint8_t type, uint8_t value)
{
    return write_attr(dst, ep, cluster, attr, type, value, 1, 0);
}

esp_err_t zb_write_u16(uint16_t dst, uint8_t ep, uint16_t cluster, uint16_t attr, uint8_t type, uint16_t value)
{
    return write_attr(dst, ep, cluster, attr, type, value, 2, 0);
}

esp_err_t zb_write_attr(uint16_t dst, uint8_t ep, uint16_t cluster, uint16_t attr, uint8_t type, uint32_t value,
                        uint8_t size, uint16_t manuf)
{
    return write_attr(dst, ep, cluster, attr, type, value, size, manuf);
}

static void bind_cb(const ezb_zdp_bind_req_result_t *r, void *ctx)
{
    uint32_t c = (uint32_t)(uintptr_t)ctx;
    ESP_LOGI(TAG, "bind 0x%04x cluster 0x%04x -> here: error %d, status %d", (unsigned)(c >> 16), (unsigned)(c & 0xFFFF),
             r ? (int)r->error : -1, r && r->rsp ? (int)r->rsp->status : -1);
}

// The device sends changes of these attributes of one cluster on its own: a binding of the cluster to our endpoint,
// then the reporting configuration in one command (what Zigbee2MQTT does when it sets a device up). Without it a plug
// switched by hand stays unseen (2026-11-12). change: smallest change that is reported, for analog attributes.
typedef struct {
    uint16_t attr;
    uint8_t type;
    uint16_t min_s, max_s;
    uint32_t change;
} rep_t;

static esp_err_t report_cluster(uint16_t dst, uint8_t ep, uint16_t cluster, const rep_t *r, int n)
{
    static ezb_zcl_config_report_record_t rec[4];   // touched only under the stack lock
    if (!s_up) {
        return ESP_ERR_INVALID_STATE;
    }
    if (n < 1 || n > 4) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!esp_zigbee_lock_acquire(pdMS_TO_TICKS(1000))) {
        return ESP_ERR_TIMEOUT;
    }
    ezb_extaddr_t dev, me;
    ezb_err_t e = ezb_address_extended_by_short(dst, &dev);
    if (e == EZB_ERR_NONE) {
        ezb_get_extended_address(&me);
        ezb_zdo_bind_req_t b = {0};
        b.dst_nwk_addr = dst;
        b.field.src_addr = dev;
        b.field.src_ep = ep;
        b.field.cluster_id = cluster;
        b.field.dst_addr_mode = EZB_ADDR_MODE_EXT;
        b.field.dst_addr.extended_addr = me;
        b.field.dst_ep = ZB_EP;
        b.cb = bind_cb;
        b.user_ctx = (void *)(uintptr_t)((uint32_t)dst << 16 | cluster);
        e = ezb_zdo_bind_req(&b);
    }
    if (e == EZB_ERR_NONE) {
        ezb_zcl_config_report_cmd_t cmd = {.cmd_ctrl = general_ctrl(dst, ep, cluster)};
        memset(rec, 0, sizeof rec);
        for (int i = 0; i < n; i++) {
            rec[i].direction = EZB_ZCL_REPORTING_SEND;
            rec[i].attr_id = r[i].attr;
            rec[i].client.attr_type = r[i].type;
            rec[i].client.min_interval = r[i].min_s;
            rec[i].client.max_interval = r[i].max_s;
            memcpy(rec[i].client.reportable_change.data, &r[i].change, sizeof r[i].change);   // little endian
        }
        cmd.payload.record_number = (uint16_t)n;
        cmd.payload.record_field = rec;
        e = ezb_zcl_config_report_cmd_req(&cmd);
    }
    esp_zigbee_lock_release();
    return esp_zigbee_err_to_esp(e);
}

esp_err_t zb_report(uint16_t dst, uint8_t ep, uint16_t cluster, uint16_t attr, uint8_t type, uint16_t min_s,
                    uint16_t max_s, uint32_t change)
{
    const rep_t r = {attr, type, min_s, max_s, change};
    return report_cluster(dst, ep, cluster, &r, 1);
}

// Everything a device of our classes should send on its own: On/Off at once and at least every 10 min, brightness
// and colour temperature on a change of 1, battery every 1 to 6 h or on a change of 1 %. A remote has On/Off and Level
// as client clusters: there the binding is what sends its button commands here. A device without a cluster answers
// "unsupported"; that costs nothing. ESP_OK: every request went out (the answers come later, see bind_cb).
esp_err_t zb_setup_reports(uint16_t dst, uint8_t ep)
{
    // colour: temperature, CurrentX/CurrentY (at most once a second, a transition moves them) and ColorMode
    // (the datapoints of the colour class, C005)
    static const rep_t colour[] = {{0x0007, 0x21, 1, 600, 1}, {0x0003, 0x21, 1, 600, 1}, {0x0004, 0x21, 1, 600, 1},
                                   {0x0008, 0x30, 1, 600, 0}};
    esp_err_t a = zb_report(dst, ep, 0x0006, 0x0000, 0x10, 0, 600, 0);
    esp_err_t b = zb_report(dst, ep, 0x0008, 0x0000, 0x20, 1, 600, 1);
    esp_err_t c = report_cluster(dst, ep, 0x0300, colour, 4);
    esp_err_t d = zb_report(dst, ep, 0x0001, 0x0021, 0x20, 3600, 21600, 2);
    ESP_LOGI(TAG, "0x%04x: binding and reporting asked for: on/off %s, level %s, colour %s, battery %s", dst,
             esp_err_to_name(a), esp_err_to_name(b), esp_err_to_name(c), esp_err_to_name(d));
    return a != ESP_OK ? a : b != ESP_OK ? b : c != ESP_OK ? c : d;
}

esp_err_t zb_color_xy(uint16_t dst, uint8_t ep, uint16_t x, uint16_t y, uint16_t tenths)
{
    if (!s_up) {
        return ESP_ERR_INVALID_STATE;
    }
    // options mask 0: the lamp's own Options attribute (ExecuteIfOff, the class sets it) decides while it is off
    ezb_zcl_color_control_move_to_color_cmd_t cmd = {.cmd_ctrl = cluster_ctrl(dst, ep)};
    cmd.payload.color_x = x;
    cmd.payload.color_y = y;
    cmd.payload.transition_time = tenths;
    if (!esp_zigbee_lock_acquire(pdMS_TO_TICKS(1000))) {
        return ESP_ERR_TIMEOUT;
    }
    ezb_err_t e = ezb_zcl_color_control_move_to_color_cmd_req(&cmd);
    esp_zigbee_lock_release();
    return esp_zigbee_err_to_esp(e);
}

esp_err_t zb_color_temp(uint16_t dst, uint8_t ep, uint16_t mired, uint16_t tenths)
{
    if (!s_up) {
        return ESP_ERR_INVALID_STATE;
    }
    // ExecuteIfOff for this command (OptionsMask and OptionsOverride, bit 0 each): a lamp that is off takes the colour
    // temperature and shows it when it is switched on. The class sends it before it switches the lamp on ("colour
    // temperature when switched on") and with KO 7 while the lamp is off. With the mask 0 the lamp's Options attribute
    // decided, 0 by default, the value got lost (found in a review). Whether a lamp heeds the override is the lamp's
    // affair: one of an older ZCL revision ignores the two octets as before. esp-zigbee-lib (seen with 2.0.1) puts
    // both into every frame (its ezb_zcl_color_control_move_to_color_temperature_cmd_req appends colour temperature,
    // transition time, mask and override): until now they went out as 00 00.
    ezb_zcl_color_control_move_to_color_temperature_cmd_t cmd = {.cmd_ctrl = cluster_ctrl(dst, ep)};
    cmd.payload.color_temperature_mireds = mired;
    cmd.payload.transition_time = tenths;
    cmd.payload.options_mask = 1u << EZB_ZCL_COLOR_CONTROL_OPTIONS_EXECUTE_IF_OFF;
    cmd.payload.options_override = 1u << EZB_ZCL_COLOR_CONTROL_OPTIONS_EXECUTE_IF_OFF;
    if (!esp_zigbee_lock_acquire(pdMS_TO_TICKS(1000))) {
        return ESP_ERR_TIMEOUT;
    }
    ezb_err_t e = ezb_zcl_color_control_move_to_color_temperature_cmd_req(&cmd);
    esp_zigbee_lock_release();
    return esp_zigbee_err_to_esp(e);
}

esp_err_t zb_color_temp_move(uint16_t dst, uint8_t ep, int dir, uint16_t rate, uint16_t min_mired, uint16_t max_mired)
{
    if (!s_up) {
        return ESP_ERR_INVALID_STATE;
    }
    // bounds in mired; 0: the lamp's own limits (ColorTempPhysicalMin/MaxMireds)
    ezb_zcl_color_control_move_color_temperature_cmd_t cmd = {.cmd_ctrl = cluster_ctrl(dst, ep)};
    cmd.payload.move_mode = dir > 0 ? EZB_ZCL_COLOR_CONTROL_MOVE_MODE_UP
                            : dir < 0 ? EZB_ZCL_COLOR_CONTROL_MOVE_MODE_DOWN
                                      : EZB_ZCL_COLOR_CONTROL_MOVE_MODE_STOP;
    cmd.payload.rate = dir ? rate : 0;
    cmd.payload.color_temperature_min_mireds = min_mired;
    cmd.payload.color_temperature_max_mireds = max_mired;
    if (!esp_zigbee_lock_acquire(pdMS_TO_TICKS(1000))) {
        return ESP_ERR_TIMEOUT;
    }
    ezb_err_t e = ezb_zcl_color_control_move_color_temperature_cmd_req(&cmd);
    esp_zigbee_lock_release();
    return esp_zigbee_err_to_esp(e);
}

esp_err_t zb_level_move(uint16_t dst, uint8_t ep, bool up, uint8_t rate, bool with_onoff)
{
    if (!s_up) {
        return ESP_ERR_INVALID_STATE;
    }
    ezb_zcl_level_move_cmd_t cmd = {.cmd_ctrl = cluster_ctrl(dst, ep)};
    cmd.payload.move_mode = up ? 0 : 1;
    cmd.payload.rate = rate;
    if (!esp_zigbee_lock_acquire(pdMS_TO_TICKS(1000))) {
        return ESP_ERR_TIMEOUT;
    }
    ezb_err_t e = with_onoff ? ezb_zcl_level_move_with_on_off_cmd_req(&cmd) : ezb_zcl_level_move_cmd_req(&cmd);
    esp_zigbee_lock_release();
    return esp_zigbee_err_to_esp(e);
}

esp_err_t zb_level_stop(uint16_t dst, uint8_t ep, bool with_onoff)
{
    if (!s_up) {
        return ESP_ERR_INVALID_STATE;
    }
    ezb_zcl_level_stop_cmd_t cmd = {.cmd_ctrl = cluster_ctrl(dst, ep)};
    if (!esp_zigbee_lock_acquire(pdMS_TO_TICKS(1000))) {
        return ESP_ERR_TIMEOUT;
    }
    ezb_err_t e = with_onoff ? ezb_zcl_level_stop_with_on_off_cmd_req(&cmd) : ezb_zcl_level_stop_cmd_req(&cmd);
    esp_zigbee_lock_release();
    return esp_zigbee_err_to_esp(e);
}

esp_err_t zb_identify(uint16_t dst, uint8_t ep, uint16_t seconds)
{
    if (!s_up) {
        return ESP_ERR_INVALID_STATE;
    }
    ezb_zcl_identify_cmd_t cmd = {.cmd_ctrl = cluster_ctrl(dst, ep)};
    cmd.payload.identify_time = seconds;
    if (!esp_zigbee_lock_acquire(pdMS_TO_TICKS(1000))) {
        return ESP_ERR_TIMEOUT;
    }
    ezb_err_t e = ezb_zcl_identify_identify_cmd_req(&cmd);
    esp_zigbee_lock_release();
    return esp_zigbee_err_to_esp(e);
}
