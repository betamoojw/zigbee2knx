// Host harness for main/vdev.c.
// The real source file is included unchanged; FreeRTOS, NVS, the link and Berry are stubs.
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- trace: everything observable goes into one hash (and optionally to stderr)
static uint64_t g_hash = 1469598103934665603ull;
static int g_verbose;
static unsigned long g_step, g_vfrom = ~0ul, g_vto;
static void tr_bytes(const void *p, size_t n)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) {
        g_hash = (g_hash ^ b[i]) * 1099511628211ull;
    }
}
static void tr(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void tr(const char *fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    if (n >= (int)sizeof buf) n = sizeof buf - 1;
    tr_bytes(buf, (size_t)n);
    if (g_verbose || (g_step >= g_vfrom && g_step <= g_vto)) {
        fprintf(stderr, "[%lu] %s\n", g_step, buf);
    }
}

// ---- allocation with accounting and failure injection
static size_t g_alloc_fail_at;   // fail the n-th allocation from now (0 = never)
static unsigned g_alloc_fail_pct;
static uint64_t g_rs_alloc = 88172645463325252ull;
static unsigned long g_alloc_failed;
static bool alloc_fails(void)
{
    if (g_alloc_fail_at && --g_alloc_fail_at == 0) {
        g_alloc_failed++;
        return true;
    }
    if (g_alloc_fail_pct) {
        g_rs_alloc ^= g_rs_alloc << 13; g_rs_alloc ^= g_rs_alloc >> 7; g_rs_alloc ^= g_rs_alloc << 17;
        if (g_rs_alloc % 100 < g_alloc_fail_pct) {
            g_alloc_failed++;
            return true;
        }
    }
    return false;
}
static void *stub_malloc(size_t n) { return alloc_fails() ? NULL : malloc(n); }
static void *stub_realloc(void *p, size_t n) { return alloc_fails() ? NULL : realloc(p, n); }

static void stub_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void stub_printf(const char *fmt, ...)
{
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    if (n >= (int)sizeof buf) n = sizeof buf - 1;
    tr_bytes(buf, (size_t)n);
    if (g_verbose > 1) fputs(buf, stderr);
}

#define malloc(n) stub_malloc(n)
#define realloc(p, n) stub_realloc(p, n)
#define printf(...) stub_printf(__VA_ARGS__)
#include "vdev.c"
#undef malloc
#undef realloc
#undef printf

// ---- stubs
static int64_t g_now = 10000000;
int64_t esp_timer_get_time(void) { return g_now; }
static unsigned long g_log[128];
void stub_log(char lvl, const char *tag, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    g_log[(unsigned char)lvl]++;
    (void)n;
    tr("%c %s: %s", lvl, tag, buf);
}
esp_err_t esp_read_mac(uint8_t *mac, int type)
{
    (void)type;
    static const uint8_t m[6] = {0x10, 0x20, 0x30, 0x80, 0x2b, 0x7c};
    memcpy(mac, m, 6);
    return ESP_OK;
}

// NVS in RAM: every blob in a block of its exact size
#define NKEY 64
static struct { char key[16]; uint8_t *b; size_t n; } g_nvs[NKEY];
static int g_nvs_fail;   // nvs_set_blob fails (full partition)
static int nvs_find(const char *key)
{
    for (int i = 0; i < NKEY; i++) {
        if (g_nvs[i].b && strcmp(g_nvs[i].key, key) == 0) return i;
    }
    return -1;
}
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len)
{
    (void)h;
    int i = nvs_find(key);
    if (i < 0) return ESP_ERR_NVS_NOT_FOUND;
    if (!out) { *len = g_nvs[i].n; return ESP_OK; }
    if (*len < g_nvs[i].n) return ESP_FAIL;
    memcpy(out, g_nvs[i].b, g_nvs[i].n);
    *len = g_nvs[i].n;
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *val, size_t len)
{
    (void)h;
    if (g_nvs_fail) return ESP_ERR_NVS_NOT_ENOUGH_SPACE;
    int i = nvs_find(key);
    if (i < 0) {
        for (i = 0; i < NKEY && g_nvs[i].b; i++) {}
        if (i == NKEY) return ESP_ERR_NVS_NOT_ENOUGH_SPACE;
    } else {
        free(g_nvs[i].b);
    }
    snprintf(g_nvs[i].key, sizeof g_nvs[i].key, "%s", key);
    g_nvs[i].b = malloc(len ? len : 1);
    memcpy(g_nvs[i].b, val, len);
    g_nvs[i].n = len;
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
esp_err_t nvs_get_stats(const char *p, nvs_stats_t *st) { (void)p; memset(st, 0, sizeof *st); return ESP_FAIL; }
esp_err_t nvs_erase_key(nvs_handle_t h, const char *key)
{
    (void)h;
    int i = nvs_find(key);
    if (i < 0) return ESP_ERR_NVS_NOT_FOUND;
    free(g_nvs[i].b);
    g_nvs[i].b = NULL;
    return ESP_OK;
}
struct nvs_opaque_iterator_t { int i; };
static struct nvs_opaque_iterator_t g_it;
static int nvs_it_from(int i) { while (i < NKEY && !g_nvs[i].b) i++; return i; }
esp_err_t nvs_entry_find(const char *p, const char *ns, nvs_type_t type, nvs_iterator_t *it)
{
    (void)p; (void)ns; (void)type;
    g_it.i = nvs_it_from(0);
    *it = g_it.i < NKEY ? &g_it : NULL;
    return *it ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}
esp_err_t nvs_entry_next(nvs_iterator_t *it)
{
    (*it)->i = nvs_it_from((*it)->i + 1);
    if ((*it)->i >= NKEY) { *it = NULL; return ESP_ERR_NVS_NOT_FOUND; }
    return ESP_OK;
}
esp_err_t nvs_entry_info(const nvs_iterator_t it, nvs_entry_info_t *info)
{
    memset(info, 0, sizeof *info);
    snprintf(info->namespace_name, sizeof info->namespace_name, "vdev");
    snprintf(info->key, sizeof info->key, "%s", g_nvs[it->i].key);
    info->type = NVS_TYPE_BLOB;
    return ESP_OK;
}
void nvs_release_iterator(nvs_iterator_t it) { (void)it; }
esp_err_t nvs_open_from_partition(const char *p, const char *ns, int m, nvs_handle_t *h) { (void)p; (void)ns; (void)m; *h = 1; return ESP_OK; }
esp_err_t nvs_flash_init_partition(const char *p) { (void)p; return ESP_OK; }
esp_err_t nvs_flash_erase_partition(const char *p) { (void)p; return ESP_OK; }

// FreeRTOS
struct stub_queue { unsigned n, size, head, count; uint8_t *buf; };
QueueHandle_t xQueueCreate(UBaseType_t n, UBaseType_t size)
{
    QueueHandle_t q = calloc(1, sizeof *q);
    q->n = n; q->size = size; q->buf = calloc(n, size);
    return q;
}
BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t t)
{
    (void)t;
    if (q->count == q->n) return pdFALSE;
    memcpy(q->buf + ((q->head + q->count) % q->n) * q->size, item, q->size);
    q->count++;
    return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t t)
{
    (void)t;
    if (!q->count) return pdFALSE;
    memcpy(item, q->buf + q->head * q->size, q->size);
    q->head = (q->head + 1) % q->n;
    q->count--;
    return pdTRUE;
}
struct stub_sem { int depth; };
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return calloc(1, sizeof(struct stub_sem)); }
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t t)
{
    (void)t;
    if (s->depth) { fprintf(stderr, "HARNESS: mutex taken twice by the same thread (self-deadlock)\n"); abort(); }
    s->depth++;
    return pdTRUE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t s) { s->depth--; return pdTRUE; }
BaseType_t xTaskCreate(void (*fn)(void *), const char *name, uint32_t st, void *arg, UBaseType_t prio, TaskHandle_t *h)
{
    (void)fn; (void)name; (void)st; (void)arg; (void)prio; (void)h;
    return pdTRUE;
}

// link
static unsigned long g_tx_n, g_tx_fail, g_ack_calls;
static unsigned g_txfail_pct;
static uint64_t g_rs_tx = 0x9E3779B97F4A7C15ull;
static struct { uint16_t src, dst; bool group; uint8_t prio, len, tpdu[KNX_TPDU_MAX]; } g_last_tx[8];
static int g_last_tx_n;
bool knx_link_send(uint16_t src, uint16_t dst, bool group, uint8_t prio, const uint8_t *tpdu, uint8_t len)
{
    if (!tpdu || len < 1 || len > KNX_TPDU_MAX) {
        fprintf(stderr, "HARNESS: knx_link_send with len %u\n", len);
        abort();
    }
    char h[2 * KNX_TPDU_MAX + 1];
    for (int i = 0; i < len; i++) sprintf(h + 2 * i, "%02x", tpdu[i]);
    g_rs_tx ^= g_rs_tx << 13; g_rs_tx ^= g_rs_tx >> 7; g_rs_tx ^= g_rs_tx << 17;
    bool ok = !(g_txfail_pct && g_rs_tx % 100 < g_txfail_pct);
    tr("TX %04x->%04x %c p%u %s%s", src, dst, group ? 'G' : 'I', prio, h, ok ? "" : " (queue full)");
    if (!ok) { g_tx_fail++; return false; }
    g_tx_n++;
    if (g_last_tx_n < 8) {
        g_last_tx[g_last_tx_n].src = src; g_last_tx[g_last_tx_n].dst = dst; g_last_tx[g_last_tx_n].group = group;
        g_last_tx[g_last_tx_n].prio = prio; g_last_tx[g_last_tx_n].len = len;
        memcpy(g_last_tx[g_last_tx_n].tpdu, tpdu, len);
        g_last_tx_n++;
    }
    return true;
}
static uint16_t g_ack_ia[KNX_ACK_IA_MAX], g_ack_ga[KNX_ACK_GA_MAX];
static int g_ack_nia, g_ack_nga;
void knx_link_ack_lists(const uint16_t *ia, int nia, const uint16_t *ga, int nga, bool broadcast)
{
    if (nia < 0 || nia > KNX_ACK_IA_MAX || nga < 0 || nga > KNX_ACK_GA_MAX) {
        fprintf(stderr, "HARNESS: ack lists %d/%d\n", nia, nga);
        abort();
    }
    memcpy(g_ack_ia, ia, (size_t)nia * 2);
    memcpy(g_ack_ga, ga, (size_t)nga * 2);
    g_ack_nia = nia; g_ack_nga = nga;
    g_ack_calls++;
    tr("ACKLISTS %d ia, %d ga, bc %d", nia, nga, broadcast);
    tr_bytes(ia, (size_t)nia * 2);
    tr_bytes(ga, (size_t)nga * 2);
}
void knx_link_sink(knx_sink_t s) { (void)s; }

// pair, zlink, bvm
void pair_knx_probe(uint16_t src, uint16_t target, int own) { tr("PAIR probe %04x %04x %d", src, target, own); }
static unsigned long g_zlink_del;
void zlink_del(int inst) { g_zlink_del++; tr("ZLINK del %d", inst); }
void zlink_pause(int inst) { g_zlink_del++; tr("ZLINK del %d", inst); }   // the same trace: the fuzz hash stays
void zlink_reread(int inst) { tr("ZLINK reread %d", inst); }
void zlink_announced(int inst) { tr("ZLINK announced %d", inst); }
static int g_sleepy_calls, g_sleepy_last = -1;
bool zlink_is_off(int inst) { (void)inst; return false; }
void zlink_discover(uint16_t addr) { tr("ZLINK discover %04x", addr); }
void zlink_gone(int inst, bool gone) { tr("ZLINK gone %d %d", inst, gone); }
void zlink_sleepy(int inst, bool sleepy) { g_sleepy_calls++; g_sleepy_last = sleepy; tr("ZLINK sleepy %d %d", inst, sleepy); }
void pair_knx_poll(uint16_t src) { tr("PAIR poll %04x", src); }
static int g_assigned_n, g_unloaded_n, g_unloaded_slot = -1;
static uint16_t g_unloaded_old;
void pair_knx_assigned(int slot, uint16_t ia) { g_assigned_n++; tr("PAIR assigned %d %04x", slot, ia); }
void pair_knx_unloaded(int slot, uint16_t old) { g_unloaded_n++; g_unloaded_slot = slot; g_unloaded_old = old; tr("PAIR unloaded %d %04x", slot, old); }
static int g_prog_ended_n, g_prog_ended_slot = -1;
void pair_knx_prog_ended(int slot) { g_prog_ended_n++; g_prog_ended_slot = slot; tr("PAIR prog ended %d", slot); }
// gw.c: the gateway's own device (slot VDEV_GW)
void gw_params(const uint8_t *p, size_t n, uint16_t ia) { tr("GW params %u B ia %04x", (unsigned)n, ia); (void)p; }
void gw_ia(uint16_t ia) { tr("GW ia %04x", ia); }
void gw_knx_write(int ko, const uint8_t *v, int n) { tr("GW kw %d %u", ko, n > 0 ? v[n - 1] : 0); }
static int g_zlink_fail;
static int g_zset_n, g_zset_ep;
int zlink_set(int inst, int k, uint16_t addr, uint8_t ep) { g_zset_n++; g_zset_ep = ep; tr("ZLINK set %d %d %04x %u", inst, k, addr, ep); return g_zlink_fail ? -1 : 0; }
esp_err_t zlink_knx_write(int inst, int ko, const char *hex)
{
    tr("ZLINK kw %d %d %s", inst, ko, hex);
    return ESP_OK;
}
static int g_tpl_mode;   // 0 none, 1 slot 2, 2 refused
int bvm_template(const uint8_t *seg, size_t n, kgt_info_t *info, char *why, size_t wn)
{
    volatile uint8_t x = 0;
    for (size_t i = 0; i < n; i++) x ^= seg[i];   // touch all of it: must be readable
    memset(info, 0, sizeof *info);
    why[0] = 0;
    if (g_tpl_mode == 1) {
        info->params = 10; info->text_len = 100; info->z = 50; info->payload = 200; info->us = 55000;
        strcpy(info->fpr, "91a09904f3083dc6"); strcpy(info->digest, "c35750c2919313d8");
        return 2;
    }
    if (g_tpl_mode == 2) { info->err = KNX_ERR_CONFIG; snprintf(why, wn, "refused by the harness"); return BVM_REFUSED; }
    return BVM_NO_TEMPLATE;
}
// The check at LoadCompleted: refuses with g_tchk_err if set, else it finds the template of mode 1 and no
// template otherwise. Mode 2 passes here on purpose: it is the template that is refused only at the restart.
static uint8_t g_tchk_err;
static unsigned long g_tchk_calls;
static size_t g_tchk_extra;
int bvm_template_check(const uint8_t *seg, size_t n, size_t free_extra, kgt_info_t *info, char *why, size_t wn)
{
    volatile uint8_t x = 0;
    for (size_t i = 0; i < n; i++) x ^= seg[i];   // touch all of it: must be readable
    memset(info, 0, sizeof *info);
    why[0] = 0;
    g_tchk_calls++;
    g_tchk_extra = free_extra;
    tr("BVM check %u B, %u B given back", (unsigned)n, (unsigned)free_extra);
    if (g_tchk_err) { info->err = g_tchk_err; snprintf(why, wn, "refused by the harness check"); return BVM_REFUSED; }
    if (g_tpl_mode == 1) { info->params = 10; strcpy(info->digest, "c35750c2919313d8"); return 0; }
    return BVM_NO_TEMPLATE;
}
size_t bvm_slot_heap(int k) { return k >= 0 ? 30000 : 0; }
const char *bvm_slot_digest(int k) { return k >= 2 ? "c35750c2919313d8" : ""; }
// gw.c: what the gateway tells
static unsigned long g_note_n[7];
static int g_note_kind;
static uint8_t g_note_err;
static uint16_t g_note_ia;
void gw_note(int kind, uint16_t ia, uint8_t err, const char *why)
{
    if (kind >= 1 && kind <= 6) g_note_n[kind]++;
    g_note_kind = kind; g_note_err = err; g_note_ia = ia;
    tr("GW note %d %04x %u %s", kind, ia, err, why);
}
void bvm_keep(uint32_t mask) { tr("BVM keep %x", (unsigned)mask); }
static char g_last_params[512];
static int g_params_n;
bool bvm_line(int k, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    tr("BVM line %d %s", k, buf);
    if (strstr(buf, "\"call\":\"params\"")) {
        snprintf(g_last_params, sizeof g_last_params, "%s", buf);
        g_params_n++;
    }
    return true;
}
unsigned long bvm_next_id(void) { static unsigned long id; return ++id; }
const char *bvm_slot_name(int k) { return k < 0 ? "none" : k < 2 ? "builtin" : "tpl"; }
int bvm_model_check(int k, int inst, char *model, size_t n, int *count)
{
    (void)k; (void)inst;
    snprintf(model, n, "MODEL");
    *count = 3;
    return -1;
}

// ---- random numbers: structure and the octets behind a frame's length are drawn separately
static uint64_t g_rs = 0x2545F4914F6CDD1Dull, g_rg = 0x1234567887654321ull;
static uint32_t rnd(void) { g_rs ^= g_rs << 13; g_rs ^= g_rs >> 7; g_rs ^= g_rs << 17; return (uint32_t)(g_rs >> 16); }
static uint32_t rng(uint32_t n) { return n ? rnd() % n : 0; }
static uint32_t garb(void) { g_rg ^= g_rg << 13; g_rg ^= g_rg >> 7; g_rg ^= g_rg << 17; return (uint32_t)(g_rg >> 16); }
static int g_garbage;   // 0: zeros behind len (as the link does), 1: random octets

static const uint16_t PARTNER[] = {0x11FA, 0xFFF1, 0x0001};
static const uint16_t GAPOOL[] = {0xF601, 0xF602, 0xF603, 0xF604, 0xF605, 0xF606, 0x0901, 0x0000, 0x7FFF};
static uint8_t g_seq[VDEV_MAX][3];   // what the harness thinks the next sequence number is

static unsigned long g_frames, g_inv_fail;

static void invariants(const char *where)
{
    for (int i = 0; i < VDEV_MAX; i++) {
        vdev_t *d = &s_d[i];
        if (!d->used) continue;
        for (int o = 1; o <= 5; o++) {
            lobj_t *l = &d->lo[o];
            uint32_t held = seg_held(l);
            bool ok = l->size <= held && (l->size == 0 || l->seg) && l->size <= (o == 4 ? BIG4_SIZE : SEG_SIZE) &&
                      l->cap <= (o == 4 ? BIG4_SIZE : SEG_SIZE) && l->state <= LS_ERROR && (l->cap == 0 || l->seg) &&
                      (l->state == LS_ERROR || l->err == 0);   // an error code only with the state Error
            if (l->size) {
                ok = ok && l->ref == ((o == 4 && l->size > SEG_SIZE) ? BIG4_BASE : SEG_BASE + (uint32_t)(o - 1) * SEG_SIZE);
            }
            if (l->ts1) {   // a kept template part: object 4 only, its own part before it, whole when it has a size
                ok = ok && o == 4 && l->ts1 <= NDEV && s_ts[l->ts1 - 1].refs && l->at && l->cap == l->at &&
                     (l->size == 0 || l->size == held);
            }
            if (!ok) {
                g_inv_fail++;
                fprintf(stderr, "INVARIANT (%s, step %lu): dev %d obj %d state %u size %u cap %u ref %x seg %p ts %u at %u\n",
                        where, g_step, i, o, l->state, (unsigned)l->size, (unsigned)l->cap, (unsigned)l->ref,
                        (void *)l->seg, l->ts1, (unsigned)l->at);
            }
        }
        conn_t *c = &d->c;
        if (c->qc > 4 || c->qh > 3 || c->rx_seq > 15 || c->tx_seq > 15) {
            g_inv_fail++;
            fprintf(stderr, "INVARIANT (%s, step %lu): dev %d conn qc %u qh %u\n", where, g_step, i, c->qc, c->qh);
        }
        for (int q = 0; q < 4; q++) {
            if (c->on && c->qn[q] > KNX_TPDU_MAX) {
                g_inv_fail++;
                fprintf(stderr, "INVARIANT: qn %u\n", c->qn[q]);
            }
        }
        if (d->inst != 100 + i) {
            g_inv_fail++;
            fprintf(stderr, "INVARIANT: inst %d of slot %d\n", d->inst, i);
        }
    }
    for (int t = 0; t < NDEV; t++) {   // every kept template part has as many references as objects point to it
        unsigned refs = 0;
        for (int i = 0; i < NDEV; i++) {
            for (int o = 1; o <= 5 && s_d[i].used; o++) refs += s_d[i].lo[o].ts1 == t + 1;
        }
        bool ok = refs == s_ts[t].refs && (!refs || (s_ts[t].data && s_ts[t].len && crc32_of(s_ts[t].data, s_ts[t].len) == s_ts[t].crc));
        if (!ok) {
            g_inv_fail++;
            fprintf(stderr, "INVARIANT (%s, step %lu): template part %d has %u references, %u objects point to it\n", where,
                    g_step, t, (unsigned)s_ts[t].refs, refs);
        }
    }
}

// one frame through the same path as on the stick: sink() into the queue, then the body of vdev_task()
static void pump(void)
{
    ev_t e;
    while (xQueueReceive(s_q, &e, 0) == pdTRUE) {
        xSemaphoreTake(s_mx, portMAX_DELAY);
        if (e.type == EV_FRAME) {
            s_last_rx = g_now;
            dispatch(&e.f);
        } else {
            ko_out(e.ko.inst, e.ko.ko, e.ko.v, e.ko.len, e.ko.initial, e.ko.status);
        }
        xSemaphoreGive(s_mx);
    }
}

static void feed(uint16_t src, uint16_t dst, bool group, bool own, const uint8_t *tpdu, int len)
{
    knx_frame_t f = {.src = src, .dst = dst, .group = group, .own = own, .len = (uint8_t)len};
    memcpy(f.tpdu, tpdu, (size_t)len);
    for (int i = len; i < KNX_TPDU_MAX; i++) {
        f.tpdu[i] = g_garbage ? (uint8_t)garb() : 0;
    }
    f.ext = len > KNX_TPDU_STD;
    char h[2 * KNX_TPDU_MAX + 1];
    for (int i = 0; i < len; i++) sprintf(h + 2 * i, "%02x", tpdu[i]);
    tr("RX %04x->%04x %c%s %s", src, dst, group ? 'G' : 'I', own ? " own" : "", h);
    g_frames++;
    g_last_tx_n = 0;
    sink(&f);
    pump();
    invariants("frame");
}

static uint16_t dev_ia(int i) { return s_d[i].ia; }

static int pick_dev(void)
{
    int n = 0, idx[VDEV_MAX];
    for (int i = 0; i < VDEV_MAX; i++) {
        if (s_d[i].used) idx[n++] = i;
    }
    return n ? idx[rng((uint32_t)n)] : -1;
}

// APDU into tpdu[], returns the length; kind < 0: random service
static int make_apdu(uint8_t *t, int dev)
{
    static const uint8_t PIDS[] = {1, 5, 6, 7, 11, 12, 13, 14, 15, 23, 25, 27, 54, 56, 57, 58, 78, 83, 0, 2, 200};
    vdev_t *d = &s_d[dev];
    int len = 2;
    uint16_t a = 0;
    int obj = rng(10) < 8 ? (int)rng(6) : (int)rng(256);
    int pid = rng(10) < 9 ? PIDS[rng(sizeof PIDS)] : (int)rng(256);
    int count = rng(10) < 7 ? 1 : (int)rng(16);
    int start = rng(10) < 6 ? 1 : rng(10) < 5 ? 0 : rng(10) < 8 ? (int)rng(40) : (int)rng(4096);
    memset(t, 0, KNX_TPDU_MAX);
    switch (rng(14)) {
    case 0: a = (uint16_t)(0x300 | (rng(4) ? 0 : rng(64))); break;
    case 1: a = 0x3D1; t[2] = 0; t[3] = 0xFF; t[4] = 0xFF; t[5] = 0xFF; t[6] = 0xFF; len = 7; break;
    case 2:
    case 3:
        a = 0x3D5; t[2] = (uint8_t)obj; t[3] = (uint8_t)pid; t[4] = (uint8_t)(count << 4 | start >> 8); t[5] = (uint8_t)start; len = 6;
        break;
    case 4:
    case 5:
    case 6: {
        a = 0x3D7; t[2] = (uint8_t)obj; t[3] = (uint8_t)pid; t[4] = (uint8_t)(count << 4 | start >> 8); t[5] = (uint8_t)start;
        int n = rng(8) ? 10 : (int)rng(11);
        for (int i = 0; i < n; i++) t[6 + i] = (uint8_t)rnd();
        if (pid == 5 || rng(4) == 0) {
            static const uint32_t SZ[] = {0, 1, 2, 3, 14, 26, 100, 1023, 1024, 1025, 3446, 16383, 16384, 16385, 0x10000, 0xFFFFFFFF};
            t[6] = rng(10) < 9 ? (uint8_t)(1 + rng(4)) : (uint8_t)rng(8);
            if (t[6] == 3) {
                uint32_t sz = rng(6) ? SZ[rng(16)] : rnd();
                t[7] = rng(10) ? 0x0B : (uint8_t)rnd();
                t[8] = (uint8_t)(sz >> 24); t[9] = (uint8_t)(sz >> 16); t[10] = (uint8_t)(sz >> 8); t[11] = (uint8_t)sz;
                t[12] = (uint8_t)rng(3); t[13] = (uint8_t)rnd(); t[14] = t[15] = 0;
            }
        }
        len = 6 + n;
        break;
    }
    case 7: a = 0x3D8; t[2] = (uint8_t)obj; t[3] = rng(2) ? 0 : (uint8_t)pid; t[4] = (uint8_t)rng(rng(2) ? 8 : 256); len = 5; break;
    case 8: a = rng(2) ? 0x2C7 : 0x2C8; t[2] = (uint8_t)obj; t[3] = (uint8_t)pid; t[4] = (uint8_t)rnd(); len = 4 + (int)rng(3); break;
    case 9:
    case 10:
    case 11: {
        bool wr = rng(3) != 0;
        bool lng = rng(5) == 0;   // as an extended frame brings it: up to 63 octets (6-bit count)
        int cnt = rng(10) < 8 ? 1 + (int)rng(lng ? 63 : 12) : (int)rng(64);
        int o = 1 + (int)rng(5);
        lobj_t *l = &d->lo[o];
        uint32_t base = l->size ? l->ref : SEG_BASE + (uint32_t)(o - 1) * SEG_SIZE, addr;
        switch (rng(6)) {
        case 0: addr = base + rng(l->size + 4); break;
        case 1: addr = base + l->size - rng(16); break;
        case 2: addr = base - rng(16); break;
        case 3: addr = rnd() & 0xFFFF; break;
        default: addr = base + rng(l->size ? l->size : 1); break;
        }
        addr &= 0xFFFF;
        a = (uint16_t)((wr ? 0x280 : 0x200) | cnt);
        t[2] = (uint8_t)(addr >> 8); t[3] = (uint8_t)addr;
        len = 4;
        if (wr) {
            int n = lng ? (rng(10) < 8 ? cnt : (int)rng(64)) : rng(10) < 8 ? (cnt > 12 ? 12 : cnt) : (int)rng(13);
            for (int i = 0; i < n; i++) t[4 + i] = (uint8_t)rnd();
            len = 4 + n;
        }
        break;
    }
    case 12: a = (uint16_t)(0x180 | rng(64)); t[2] = (uint8_t)rnd(); len = 3; break;
    default:
        if (rng(4) == 0) {
            a = (uint16_t)(0x380 | (rng(4) ? 0 : rng(64)));
        } else {
            a = (uint16_t)rng(1024);
            len = 2 + (int)rng(rng(4) ? 15 : KNX_TPDU_MAX - 1);
            for (int i = 2; i < len; i++) t[i] = (uint8_t)rnd();
        }
        break;
    }
    t[0] = (uint8_t)(a >> 8);
    t[1] = (uint8_t)a;
    if (rng(12) == 0) len = 1 + (int)rng((uint32_t)len);   // cut short
    return len;
}

// a download the way ETS does it, connectionless (apdu() is the same for both modes)
static void prop_write5(int dev, int obj, const uint8_t *ev)
{
    uint8_t t[16] = {0x03, 0xD7, (uint8_t)obj, 5, 0x10, 0x01};
    memcpy(t + 6, ev, 10);
    feed(PARTNER[0], dev_ia(dev), false, false, t, 16);
}
static void mem_write(int dev, uint32_t addr, const uint8_t *p, int n)
{
    while (n > 0) {
        int c = n > 12 ? 12 : n;
        uint8_t t[16] = {0x02, (uint8_t)(0x80 | c), (uint8_t)(addr >> 8), (uint8_t)addr};
        memcpy(t + 4, p, (size_t)c);
        feed(PARTNER[0], dev_ia(dev), false, false, t, 4 + c);
        addr += (uint32_t)c; p += c; n -= c;
    }
}
// A parameter segment as a product with a template brings it (kgt.c): parameters (different every time), base64
// text (one of three), trailer "KGTE" + text offset, text length, parameter length. Same variant = same template part.
#define TPL_SEG_MAX 400
static int tpl_seg(int v, uint8_t *out)
{
    static const int P[3] = {10, 10, 22}, L[3] = {250, 250, 300};
    static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int p = P[v], l = L[v], n = p + l + 16;
    if (!out) return n;
    for (int i = 0; i < p; i++) out[i] = (uint8_t)rnd();
    for (int i = 0; i < l; i++) out[p + i] = (uint8_t)B64[(i * (v + 3) + v) % 64];
    char tr[17];
    snprintf(tr, sizeof tr, "KGTE%04X%04X%04X", (unsigned)p, (unsigned)l, (unsigned)p);
    memcpy(out + p + l, tr, 16);
    return n;
}

static void download(int dev, bool sane)
{
    static const uint8_t UNLOAD[10] = {4}, START[10] = {1}, DONE[10] = {2};
    uint8_t tab[3][64] = {{0}};
    int sz[3];
    int nga = 1 + (int)rng(8), nas = 1 + (int)rng(10), nko = 1 + (int)rng(KO_MAX + 2);
    // address table
    tab[0][0] = 0; tab[0][1] = (uint8_t)(sane ? (uint32_t)nga : rng(2) ? rng(600) : (uint32_t)nga);
    if (!sane && rng(4) == 0) tab[0][0] = (uint8_t)rnd();
    for (int i = 0; i < nga; i++) {
        uint16_t g = GAPOOL[rng(sane ? 7 : 9)];
        tab[0][2 + 2 * i] = (uint8_t)(g >> 8); tab[0][3 + 2 * i] = (uint8_t)g;
    }
    sz[0] = 2 + 2 * nga;
    // association table
    tab[1][0] = 0; tab[1][1] = (uint8_t)(sane ? (uint32_t)nas : rng(2) ? rng(300) : (uint32_t)nas);
    if (!sane && rng(4) == 0) tab[1][0] = (uint8_t)rnd();
    for (int i = 0; i < nas; i++) {
        int ts = sane ? 1 + (int)rng((uint32_t)nga) : (int)rng(rng(2) ? 12 : 65536);
        int as = sane ? 1 + (int)rng((uint32_t)nko) : (int)rng(rng(2) ? 20 : 65536);
        tab[1][2 + 4 * i] = (uint8_t)(ts >> 8); tab[1][3 + 4 * i] = (uint8_t)ts;
        tab[1][4 + 4 * i] = (uint8_t)(as >> 8); tab[1][5 + 4 * i] = (uint8_t)as;
    }
    sz[1] = 2 + 4 * nas;
    // group object table: flags octet, type octet
    tab[2][0] = 0; tab[2][1] = (uint8_t)(sane ? (uint32_t)nko : rng(2) ? rng(300) : (uint32_t)nko);
    if (!sane && rng(4) == 0) tab[2][0] = (uint8_t)rnd();
    for (int i = 0; i < nko; i++) {
        tab[2][2 + 2 * i] = sane ? (uint8_t)(rng(2) ? 0x17 : 0x4F) : (uint8_t)rnd();
        tab[2][3 + 2 * i] = sane ? (uint8_t)rng(11) : (uint8_t)rnd();
    }
    sz[2] = 2 + 2 * nko;
    if (!sane) {
        for (int k = 0; k < 3; k++) {
            if (rng(4) == 0) sz[k] = 1 + (int)rng(63);   // segment shorter or longer than the table says
        }
    }
    for (int o = 5; o >= 1; o--) prop_write5(dev, o, UNLOAD);
    static const int ORDER[3] = {3, 1, 2};
    for (int k = 0; k < 3; k++) {
        int o = ORDER[k], n = sz[o - 1];
        uint8_t al[10] = {3, 0x0B, 0, 0, (uint8_t)(n >> 8), (uint8_t)n, (uint8_t)rng(2), 0, 0, 0};
        prop_write5(dev, o, START);
        prop_write5(dev, o, al);
    }
    {
        int tv = rng(2) ? (int)rng(3) : -1;   // a product with a template behind its parameters, or none
        int n = tv >= 0 ? tpl_seg(tv, NULL) : rng(3) ? 10 + (int)rng(60) : 1025 + (int)rng(3000);
        uint8_t al[10] = {3, 0x0B, 0, 0, (uint8_t)(n >> 8), (uint8_t)n, 1, 0, 0, 0};
        uint8_t pv[16] = {0x03, 0xD7, 4, 13, 0x10, 0x01, 0x00, 0xFA, 0xFE, (uint8_t)(0x20 + rng(3)), 0x02};
        prop_write5(dev, 4, START);
        prop_write5(dev, 4, al);
        feed(PARTNER[0], dev_ia(dev), false, false, pv, 11);
        if (tv >= 0) {
            uint8_t s[TPL_SEG_MAX];
            int m = tpl_seg(tv, s);
            if (!sane && rng(3) == 0) m -= 1 + (int)rng(40);   // the trailer or part of the text does not arrive
            mem_write(dev, (uint32_t)n > SEG_SIZE ? BIG4_BASE : SEG_BASE + 3 * SEG_SIZE, s, m);
        }
    }
    for (int k = 0; k < 3; k++) {
        int o = ORDER[k];
        mem_write(dev, SEG_BASE + (uint32_t)(o - 1) * SEG_SIZE, tab[o - 1], sz[o - 1] > 64 ? 64 : sz[o - 1]);
    }
    if (!sane && rng(2)) {   // a second allocation while loading: zero, too big, the limits
        static const uint32_t SZ[] = {0, 1, 2, 1023, 1024, 1025, 16383, 16384, 16385, 0x10000, 0xFFFFFFFF, 0x80000000};
        int o = 1 + (int)rng(5);
        uint32_t n = SZ[rng(12)];
        uint8_t al[10] = {3, rng(8) ? 0x0B : 0x0A, (uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n, (uint8_t)rng(2), (uint8_t)rnd(), 0, 0};
        prop_write5(dev, o, START);
        prop_write5(dev, o, al);
        if (rng(2)) {   // read the table as an array and the stamp while it is in that state
            for (int q = 0; q < 6; q++) {
                uint8_t r[6] = {0x03, 0xD5, (uint8_t)(1 + rng(4)), rng(2) ? 23 : 27, (uint8_t)(rng(16) << 4), (uint8_t)rng(40)};
                feed(PARTNER[0], dev_ia(dev), false, false, r, 6);
            }
        }
    }
    for (int o = 4; o >= 1; o--) prop_write5(dev, o, DONE);
    for (int q = 0; q < 8; q++) {   // PID_TABLE as an array, in and out of range
        uint8_t r[6] = {0x03, 0xD5, (uint8_t)(1 + rng(2)), 23, (uint8_t)(rng(16) << 4), (uint8_t)rng(rng(2) ? 12 : 256)};
        feed(PARTNER[0], dev_ia(dev), false, false, r, 6);
    }
    if (rng(3)) {
        uint8_t r[2] = {0x03, 0x80};
        feed(PARTNER[0], dev_ia(dev), false, false, r, 2);
    }
}

typedef struct {
    uint16_t ia, zaddr;
    uint8_t dc, zep;
    struct { uint8_t state, run, pv[5], err; uint32_t size; uint8_t *c; } o[5];
    bool used, cap_known, rx_on;
} view_t;

static void view_take(view_t *v)
{
    for (int i = 0; i < VDEV_MAX; i++) {
        vdev_t *d = &s_d[i];
        memset(&v[i], 0, sizeof v[i]);
        v[i].used = d->used;
        if (!d->used) continue;
        v[i].ia = d->ia; v[i].zaddr = d->zaddr; v[i].dc = d->device_control; v[i].zep = d->zep;
        v[i].cap_known = d->cap_known; v[i].rx_on = d->rx_on;
        for (int o = 1; o <= 5; o++) {
            lobj_t *l = &d->lo[o];
            v[i].o[o - 1].state = l->state == LS_LOADING ? LS_UNLOADED : l->state;
            v[i].o[o - 1].run = l->run_state;
            v[i].o[o - 1].err = l->state == LS_ERROR ? l->err : 0;
            memcpy(v[i].o[o - 1].pv, l->pv, 5);
            // only a loaded object is stored with its content: Loading and Error come back without a segment
            uint32_t keep = l->state == LS_LOADED ? l->size : 0;
            v[i].o[o - 1].size = keep;
            v[i].o[o - 1].c = malloc(keep ? keep : 1);
            if (keep) seg_read(l, 0, v[i].o[o - 1].c, keep);   // a kept template part included
        }
    }
}
static int view_cmp(const view_t *v, const char *what)
{
    int bad = 0;
    for (int i = 0; i < VDEV_MAX; i++) {
        vdev_t *d = &s_d[i];
        if (v[i].used != d->used) { fprintf(stderr, "%s: dev %d used %d -> %d\n", what, i, v[i].used, d->used); bad++; continue; }
        if (!d->used) continue;
        if (v[i].ia != d->ia || v[i].zaddr != d->zaddr || v[i].dc != d->device_control || v[i].zep != d->zep ||
            v[i].cap_known != d->cap_known || v[i].rx_on != d->rx_on) {
            fprintf(stderr, "%s: dev %d head differs\n", what, i); bad++;
        }
        for (int o = 1; o <= 5; o++) {
            lobj_t *l = &d->lo[o];
            uint8_t *cur = malloc(l->size ? l->size : 1);
            if (l->size) seg_read(l, 0, cur, l->size);
            bool same = v[i].o[o - 1].size == l->size && (!l->size || !memcmp(v[i].o[o - 1].c, cur, l->size));
            if (v[i].o[o - 1].state != l->state || v[i].o[o - 1].run != l->run_state || memcmp(v[i].o[o - 1].pv, l->pv, 5) || !same ||
                v[i].o[o - 1].err != l->err) {
                fprintf(stderr, "%s (step %lu): dev %d obj %d differs: state %u/%u size %u/%u content %s\n", what, g_step, i, o,
                        v[i].o[o - 1].state, l->state, (unsigned)v[i].o[o - 1].size, (unsigned)l->size,
                        (v[i].o[o - 1].size == l->size && !same) ? "DIFFERENT" : "-");
                bad++;
            }
            free(cur);
        }
    }
    return bad;
}
static void view_free(view_t *v)
{
    for (int i = 0; i < VDEV_MAX; i++) {
        for (int o = 0; o < 5; o++) free(v[i].o[o].c);
    }
}

static unsigned long g_reboots, g_reboot_diff, g_corrupt_ok, g_corrupt_refused;

static void wipe_ram(void)
{
    for (int i = 0; i < NDEV; i++) {   // the gateway too: a test may have loaded it
        for (int o = 1; o <= 5; o++) free(s_d[i].lo[o].seg);
    }
    memset(s_d, 0, sizeof s_d);
    for (int t = 0; t < NDEV; t++) free(s_ts[t].data);   // RAM is gone, the blobs stay
    memset(s_ts, 0, sizeof s_ts);
    memset(s_tref, 0, sizeof s_tref);
    memset(g_seq, 0, sizeof g_seq);
}

static void reboot(bool corrupt)
{
    view_t v[VDEV_MAX];
    unsigned keep_pct = g_alloc_fail_pct;
    int keep_nvs = g_nvs_fail;
    g_alloc_fail_pct = 0;
    g_nvs_fail = 0;   // the comparison below is about format and restore(), not about a full partition
    for (int i = 0; i < VDEV_MAX; i++) {
        if (s_d[i].used) save(&s_d[i]);   // as timers() would once the bus is quiet
    }
    view_take(v);
    wipe_ram();
    if (corrupt) {
        for (int k = 0; k < NKEY; k++) {
            if (!g_nvs[k].b || rng(2)) continue;
            switch (rng(5)) {
            case 0: g_nvs[k].b[rng((uint32_t)g_nvs[k].n)] ^= (uint8_t)(1u << rng(8)); break;
            case 1: for (int j = 0; j < 8; j++) g_nvs[k].b[rng((uint32_t)g_nvs[k].n)] = (uint8_t)rnd(); break;
            case 2: {   // shorter, in a block of the new exact size
                size_t n = rng((uint32_t)g_nvs[k].n + 1);
                uint8_t *nb = malloc(n ? n : 1);
                memcpy(nb, g_nvs[k].b, n);
                free(g_nvs[k].b); g_nvs[k].b = nb; g_nvs[k].n = n;
                break;
            }
            case 3: {   // longer
                size_t n = g_nvs[k].n + 1 + rng(64);
                uint8_t *nb = malloc(n);
                for (size_t j = 0; j < n; j++) nb[j] = (uint8_t)rnd();
                memcpy(nb, g_nvs[k].b, g_nvs[k].n);
                free(g_nvs[k].b); g_nvs[k].b = nb; g_nvs[k].n = n;
                break;
            }
            default:   // a size field
                if (g_nvs[k].n >= sizeof(rec_t)) {
                    size_t off = 12 + 12 * rng(5) + 8;
                    uint32_t x = rng(2) ? rnd() : rng(20000);
                    memcpy(g_nvs[k].b + off, &x, 4);
                }
                break;
            }
        }
    }
    for (int i = 0; i < VDEV_MAX; i++) {
        bool ok = restore(i);
        if (corrupt) {
            if (ok) g_corrupt_ok++; else g_corrupt_refused++;
        }
    }
    ts_sweep();   // as vdev_start(): template parts no record names go
    g_reboots++;
    if (!corrupt) {
        int bad = view_cmp(v, "REBOOT");
        if (bad) g_reboot_diff++;
    }
    view_free(v);
    publish();
    invariants("reboot");
    g_alloc_fail_pct = keep_pct;
    g_nvs_fail = keep_nvs;
}

static size_t pinned(void)
{
    size_t n = 0;
    for (int i = 0; i < VDEV_MAX; i++) {
        for (int o = 1; o <= 5; o++) n += s_d[i].lo[o].cap;
    }
    for (int t = 0; t < NDEV; t++) n += s_ts[t].refs ? s_ts[t].len : 0;
    return n;
}

#ifndef NO_MAIN
int main(int argc, char **argv)
{
    unsigned long steps = argc > 1 ? strtoul(argv[1], NULL, 0) : 100000;
    uint64_t seed = argc > 2 ? strtoull(argv[2], NULL, 0) : 1;
    g_garbage = argc > 3 ? atoi(argv[3]) : 0;
    uint64_t gseed = argc > 4 ? strtoull(argv[4], NULL, 0) : 7;
    g_alloc_fail_pct = argc > 5 ? (unsigned)atoi(argv[5]) : 0;
    g_txfail_pct = argc > 6 ? (unsigned)atoi(argv[6]) : 0;
    if (argc > 8) { g_vfrom = strtoul(argv[7], NULL, 0); g_vto = strtoul(argv[8], NULL, 0); }
    g_rs ^= seed * 0x9E3779B97F4A7C15ull;
    g_rg ^= gseed * 0xD6E8FEB86659FD93ull;
    for (int i = 0; i < 8; i++) { rnd(); garb(); }

    vdev_start();
    size_t max_pinned = 0;
    for (int i = 0; i < 6; i++) {
        int n = vdev_add(rng(4) ? (uint16_t)(0x1000 + i) : 0xFFFF, 1, (int)rng(2), NULL);
        if (n >= 0 && rng(4)) vdev_set_ia(n, (uint16_t)(0xFF01 + n));
    }
    for (g_step = 0; g_step < steps; g_step++) {
        int dev = pick_dev();
        g_now += 1000 + rng(20000);
        uint32_t what = rng(100);
        if (dev < 0 || what < 2) {
            if (rng(2) || dev < 0) {
                uint8_t ie[8] = {0x02, 0x00, 0x00, 0xff, 0xfe, 0, 0, (uint8_t)rng(6)};   // a small pool: some repeat
                int n = vdev_add(rng(4) ? (uint16_t)(0x1000 + rng(16)) : 0xFFFF, (uint8_t)(1 + rng(3)), (int)rng(2),
                                 rng(2) ? ie : NULL);
                if (n >= 0 && rng(3)) vdev_set_ia(n, (uint16_t)(0xFF01 + rng(10)));
            } else if (rng(4) == 0) {
                vdev_del((int)rng(VDEV_MAX + 2) - 1);
            }
            continue;
        }
        vdev_t *d = &s_d[dev];
        uint8_t t[KNX_TPDU_MAX] = {0};
        int p = (int)rng(3);
        uint16_t src = PARTNER[p];
        if (what < 10) {   // anything at all, now and then as long as an extended frame brings it
            int len = 1 + (int)rng(rng(4) ? 16 : KNX_TPDU_MAX);
            for (int i = 0; i < KNX_TPDU_MAX; i++) t[i] = (uint8_t)rnd();
            if (rng(2)) t[0] &= 0x03;
            bool group = rng(2);
            uint16_t dst = group ? GAPOOL[rng(9)] : rng(4) ? dev_ia(dev) : (uint16_t)rnd();
            feed(rng(8) ? src : dev_ia((int)rng(VDEV_MAX)), dst, group, rng(8) == 0, t, len);
        } else if (what < 22) {   // transport control
            static const uint8_t TP[] = {0x80, 0x80, 0x80, 0x81, 0xC2, 0xC3, 0x82, 0x83};
            uint8_t tp = TP[rng(8)];
            if (tp >= 0xC2) tp |= (uint8_t)((rng(3) ? d->c.tx_seq : rng(16)) << 2);
            t[0] = tp;
            feed(src, dev_ia(dev), false, false, t, 1 + (rng(10) ? 0 : (int)rng(3)));
        } else if (what < 62) {   // a service, connected or not
            int len = make_apdu(t, dev);
            if (rng(10) < 6) {
                if (!d->c.on || d->c.remote != src) {
                    uint8_t c = 0x80;
                    if (rng(4)) feed(src, dev_ia(dev), false, false, &c, 1);
                }
                uint8_t seq = rng(10) ? d->c.rx_seq : (uint8_t)rng(16);
                t[0] = (uint8_t)(0x40 | seq << 2 | (t[0] & 3));
                feed(src, dev_ia(dev), false, false, t, len);
                if (rng(3) && d->c.on && d->c.qc) {   // acknowledge what came back
                    uint8_t a = (uint8_t)(0xC2 | d->c.tx_seq << 2);
                    feed(src, dev_ia(dev), false, false, &a, 1);
                }
            } else {
                feed(src, dev_ia(dev), false, false, t, len);
            }
        } else if (what < 68) {   // broadcast
            memset(t, 0, sizeof t);
            int len = 2;
            switch (rng(5)) {
            case 0: t[0] = 0x01; t[1] = 0x00; break;
            case 1: t[0] = 0x00; t[1] = 0xC0; { uint16_t ia = rng(3) ? (uint16_t)(0xFF01 + rng(10)) : (uint16_t)rnd(); t[2] = (uint8_t)(ia >> 8); t[3] = (uint8_t)ia; } len = 4; break;
            case 2: t[0] = 0x03; t[1] = 0xDC; memcpy(t + 2, d->serial, 6); if (rng(4) == 0) t[7] ^= 1; len = 8; break;
            case 3: t[0] = 0x03; t[1] = 0xDE; memcpy(t + 2, d->serial, 6); { uint16_t ia = (uint16_t)(0xFF01 + rng(10)); t[8] = (uint8_t)(ia >> 8); t[9] = (uint8_t)ia; } len = 14; break;
            default: for (int i = 0; i < 16; i++) t[i] = (uint8_t)rnd(); t[0] &= 3; len = 2 + (int)rng(15); break;
            }
            if (rng(10) == 0) len = 1 + (int)rng((uint32_t)len);
            feed(src, 0, true, rng(10) == 0, t, len);
        } else if (what < 82) {   // group communication
            const uint8_t *g;
            int ng = ga_count(d, &g);
            uint16_t ga = ng && rng(4) ? be16(g + 2 * (1 + (int)rng((uint32_t)ng))) : GAPOOL[rng(9)];
            int len = 2 + (rng(2) ? 0 : (int)rng(rng(6) ? 15 : KNX_TPDU_MAX - 1));   // longer than a value: ignored
            for (int i = 0; i < KNX_TPDU_MAX; i++) t[i] = (uint8_t)rnd();
            t[0] = 0;
            switch (rng(4)) {
            case 0: t[1] = 0x00; break;                       // read
            case 1: t[0] = 0; t[1] = (uint8_t)(0x40 | rng(64)); break;   // response
            default: t[1] = (uint8_t)(0x80 | rng(64)); break; // write
            }
            bool own = rng(5) == 0;
            feed(own ? dev_ia((int)rng(VDEV_MAX)) : src, ga, true, own, t, len);
        } else if (what < 88) {   // a value from the class
            uint8_t v[KNX_KO_VAL + 1];   // one octet more than a value may take: that length is refused
            for (int i = 0; i < KNX_KO_VAL + 1; i++) v[i] = (uint8_t)rnd();
            vdev_ko_out(98 + (int)rng(14), (int)rng(KO_MAX + 3), v, (int)rng(KNX_KO_VAL + 2), rng(4) == 0, rng(2));
            pump();
            invariants("ko");
        } else if (what < 92) {   // time passes
            g_now += rng(3) ? rng(400000) : rng(8000000);
            xSemaphoreTake(s_mx, portMAX_DELAY);
            timers(g_now);
            xSemaphoreGive(s_mx);
            invariants("timers");
        } else if (what < 95) {   // console
            switch (rng(6)) {
            case 5:
                if (rng(2)) {
                    vdev_max_apdu(rng(2) ? 15 : (int)rng(80));   // PID 56; out of range leaves it
                } else {   // a device announce: whether it sleeps (bit 3), kept in the record
                    uint8_t ie[8] = {0x02, 0x00, 0x00, 0xff, 0xfe, 0, 0, (uint8_t)rng(7)};
                    vdev_set_capability(ie, (uint8_t)(rng(2) ? 0x8e : 0x80));
                }
                break;
            case 0: vdev_prog((int)rng(VDEV_MAX + 2) - 1, rng(2)); break;
            case 1: vdev_set_ia((int)rng(VDEV_MAX + 2) - 1, rng(3) ? (uint16_t)(0xFF01 + rng(10)) : 0xFFFF); break;
            case 2: vdev_print(); break;
            case 3: vdev_dump((int)rng(VDEV_MAX + 2) - 1, (int)rng(8) - 1); break;
            default: (void)vdev_any_prog(); (void)vdev_first_unbound(); (void)vdev_has_zigbee((uint16_t)(0x1000 + rng(16))); vdev_model_event(98 + (int)rng(12)); break;
            }
        } else if (what < 98) {   // a whole download
            g_tpl_mode = (int)rng(3);
            g_tchk_err = rng(6) == 0 ? (uint8_t)(1 + rng(18)) : 0;   // the check at LoadCompleted refuses
            download(dev, rng(3) != 0);
            g_tchk_err = 0;
        } else if (what < 99) {
            g_nvs_fail = rng(8) == 0;
            g_zlink_fail = rng(8) == 0;
        } else {
            reboot(rng(3) == 0);
        }
        size_t pn = pinned();
        if (pn > max_pinned) max_pinned = pn;
    }
    fprintf(stderr, "steps %lu, frames %lu, tx %lu (+%lu refused by the link), ack list updates %lu, logs I %lu W %lu E %lu, reboots %lu "
            "(%lu with a difference), damaged records taken %lu / refused %lu, allocations failed %lu, invariant failures %lu, "
            "events lost %lu, most segment RAM held %zu B, capability calls that reached a link %d\n",
            steps, g_frames, g_tx_n, g_tx_fail, g_ack_calls, g_log['I'], g_log['W'], g_log['E'], g_reboots, g_reboot_diff,
            g_corrupt_ok, g_corrupt_refused, g_alloc_failed, g_inv_fail, (unsigned long)s_ev_lost, max_pinned,
            g_sleepy_calls);
    printf("HASH %016llx\n", (unsigned long long)g_hash);
    return g_inv_fail ? 2 : 0;
}
#endif
