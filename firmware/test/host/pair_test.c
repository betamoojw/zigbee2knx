// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dirk Tostmann
// Host test of main/pair.c: what happens to the Zigbee device when its virtual device goes (points 9 and 10 there),
// and further cases (a join without an announce, a slow key exchange, an opening from elsewhere, two assignments in
// a row, a restart between unloading and removal).
// The real pair task runs on a virtual clock; the device server, Zigbee and the flash are small models that record
// what was asked of them. Ends with "pair checks: N passed, M failed".
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pair.c"

static int64_t g_now = 1000000000LL;
int64_t esp_timer_get_time(void) { return g_now; }

// ---- queue and task: pair_task() runs in run() until its queue is empty and one tick without an event has passed
struct stub_queue { int n, size, head, count; unsigned char *buf; };
QueueHandle_t xQueueCreate(UBaseType_t n, UBaseType_t size)
{
    struct stub_queue *q = calloc(1, sizeof *q);
    q->n = (int)n; q->size = (int)size; q->buf = calloc(n, size);
    return q;
}
BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t t)
{
    (void)t;
    if (q->count == q->n) return pdFALSE;
    memcpy(q->buf + (size_t)((q->head + q->count) % q->n) * q->size, item, (size_t)q->size);
    q->count++;
    return pdTRUE;
}
static jmp_buf g_jb;
static int g_idle;
BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t t)
{
    (void)t;
    if (!q->count) {
        if (g_idle-- > 0) return pdFALSE;
        longjmp(g_jb, 1);
    }
    memcpy(item, q->buf + (size_t)q->head * q->size, (size_t)q->size);
    q->head = (q->head + 1) % q->n;
    q->count--;
    return pdTRUE;
}
BaseType_t xTaskCreate(void (*fn)(void *), const char *name, uint32_t stack, void *arg, UBaseType_t prio, TaskHandle_t *h)
{
    (void)fn; (void)name; (void)stack; (void)arg; (void)prio; (void)h;
    return pdTRUE;
}
static void run(void)
{
    g_idle = 1;
    if (!setjmp(g_jb)) pair_task(NULL);
}
static void later(int ms)
{
    g_now += (int64_t)ms * 1000;
    run();
}

static int g_verbose;
static char g_lastw[400];   // the last warning, as the console shows it
void stub_log(char lvl, const char *tag, const char *fmt, ...)
{
    char b[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (lvl == 'W') snprintf(g_lastw, sizeof g_lastw, "%s", b);
    if (g_verbose) printf("    [%c %s] %s\n", lvl, tag, b);
}

// ---- flash: the keys of one namespace
static struct { char key[16]; uint8_t val[320]; size_t len; bool used; } g_kv[8];
static char g_part[20];
static int g_commits;
static int kv(const char *key, bool make)
{
    int free_i = -1;
    for (int i = 0; i < 8; i++) {
        if (g_kv[i].used && strcmp(g_kv[i].key, key) == 0) return i;
        if (!g_kv[i].used && free_i < 0) free_i = i;
    }
    if (make && free_i >= 0) {
        g_kv[free_i].used = true;
        snprintf(g_kv[free_i].key, sizeof g_kv[free_i].key, "%s", key);
        return free_i;
    }
    return -1;
}
esp_err_t nvs_open_from_partition(const char *part, const char *ns, int mode, nvs_handle_t *h)
{
    (void)ns; (void)mode;
    snprintf(g_part, sizeof g_part, "%s", part);
    *h = 1;
    return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len)
{
    (void)h;
    int i = kv(key, false);
    if (i < 0) return ESP_ERR_NVS_NOT_FOUND;
    if (g_kv[i].len > *len) return ESP_FAIL;
    memcpy(out, g_kv[i].val, g_kv[i].len);
    *len = g_kv[i].len;
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *val, size_t len)
{
    (void)h;
    int i = kv(key, true);
    if (i < 0 || len > sizeof g_kv[i].val) return ESP_FAIL;
    memcpy(g_kv[i].val, val, len);
    g_kv[i].len = len;
    return ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t h, const char *key)
{
    (void)h;
    int i = kv(key, false);
    if (i < 0) return ESP_ERR_NVS_NOT_FOUND;
    g_kv[i].used = false;
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; g_commits++; return ESP_OK; }
esp_err_t nvs_get_u16(nvs_handle_t h, const char *key, uint16_t *v) { size_t n = 2; return nvs_get_blob(h, key, v, &n); }
esp_err_t nvs_get_u8(nvs_handle_t h, const char *key, uint8_t *v) { size_t n = 1; return nvs_get_blob(h, key, v, &n); }
esp_err_t nvs_set_u16(nvs_handle_t h, const char *key, uint16_t v) { return nvs_set_blob(h, key, &v, 2); }
esp_err_t nvs_set_u8(nvs_handle_t h, const char *key, uint8_t v) { return nvs_set_blob(h, key, &v, 1); }

// ---- the device server: what pair.c needs to know of a virtual device
typedef struct { bool used, gone, prog, has_ieee; uint16_t ia, zaddr; uint8_t ieee[8]; } fdev_t;
static fdev_t g_d[KNX_VDEV_MAX];
static int g_dels[16], g_ndel;
static bool ok(int i) { return i >= 0 && i < KNX_VDEV_MAX && g_d[i].used; }
int vdev_add(uint16_t zaddr, uint8_t ep, int k, const uint8_t *ieee)
{
    (void)ep; (void)k;
    for (int i = 0; i < KNX_VDEV_MAX; i++) {
        if (!g_d[i].used) {
            memset(&g_d[i], 0, sizeof g_d[i]);
            g_d[i].used = true; g_d[i].ia = 0xFFFF; g_d[i].zaddr = zaddr;
            if (ieee) { g_d[i].has_ieee = true; memcpy(g_d[i].ieee, ieee, 8); }
            return i;
        }
    }
    return -1;
}
bool vdev_del(int i)
{
    if (!ok(i)) return false;
    g_d[i].used = false;
    if (g_ndel < 16) g_dels[g_ndel++] = i;
    return true;
}
bool vdev_prog(int i, bool on) { if (!ok(i)) return false; g_d[i].prog = on; return true; }
bool vdev_any_prog(void) { for (int i = 0; i < KNX_VDEV_MAX; i++) if (g_d[i].used && g_d[i].prog) return true; return false; }
bool vdev_unbound(int i) { return ok(i) && g_d[i].ia == 0xFFFF && g_d[i].zaddr != 0xFFFF; }
bool vdev_unprogrammed(int i) { return ok(i) && g_d[i].ia == 0xFFFF; }
int vdev_first_unbound(void) { for (int i = 0; i < KNX_VDEV_MAX; i++) if (vdev_unbound(i)) return i; return -1; }
int vdev_of_zigbee(uint16_t z) { for (int i = 0; i < KNX_VDEV_MAX; i++) if (g_d[i].used && g_d[i].zaddr == z) return i; return -1; }
int vdev_free_slots(void) { int n = 0; for (int i = 0; i < KNX_VDEV_MAX; i++) n += !g_d[i].used; return n; }
int vdev_by_ieee(const uint8_t ieee[8])
{
    for (int i = 0; i < KNX_VDEV_MAX; i++) if (g_d[i].used && g_d[i].has_ieee && memcmp(g_d[i].ieee, ieee, 8) == 0) return i;
    return -1;
}
bool vdev_seen(uint16_t zaddr, const uint8_t ieee[8])
{
    int i = vdev_by_ieee(ieee);
    if (i >= 0) { g_d[i].zaddr = zaddr; return true; }
    i = vdev_of_zigbee(zaddr);
    if (i >= 0 && !g_d[i].has_ieee) { g_d[i].has_ieee = true; memcpy(g_d[i].ieee, ieee, 8); return true; }
    return false;
}
bool vdev_ieee(int i, uint8_t ieee[8]) { if (!ok(i) || !g_d[i].has_ieee) return false; memcpy(ieee, g_d[i].ieee, 8); return true; }
int vdev_rx_on(int i) { (void)i; return -1; }   // the device record knows nothing: pair.c decides from the announce
bool vdev_zigbee(int i, uint16_t *zaddr, uint8_t *ep) { if (!ok(i) || g_d[i].zaddr == 0xFFFF) return false; *zaddr = g_d[i].zaddr; *ep = 1; return true; }
void vdev_gone(int i) { if (ok(i)) g_d[i].gone = true; }
bool vdev_is_gone(int i) { return ok(i) && g_d[i].gone; }
bool vdev_replace(int i, uint16_t zaddr, const uint8_t ieee[8])
{
    if (!ok(i)) return false;
    g_d[i].zaddr = zaddr; g_d[i].gone = false; g_d[i].has_ieee = true; memcpy(g_d[i].ieee, ieee, 8);
    return true;
}
void vdev_set_capability(const uint8_t ieee[8], uint8_t capability) { (void)ieee; (void)capability; }

// ---- Zigbee and the gateway's message: recorded
static uint16_t g_leave[32];
static int g_nleave, g_open_s = -1, g_nopen, g_nread;
esp_err_t zb_leave(uint16_t short_addr, const uint8_t *ieee) { (void)ieee; if (g_nleave < 32) g_leave[g_nleave++] = short_addr; return ESP_OK; }
esp_err_t zb_permit_join(uint8_t seconds) { g_open_s = seconds; g_nopen += seconds > 0; return ESP_OK; }
esp_err_t zb_read(uint16_t dst, uint8_t ep, uint16_t cluster, const uint16_t *attrs, int n) { (void)dst; (void)ep; (void)cluster; (void)attrs; (void)n; g_nread++; return ESP_OK; }
esp_err_t zb_identify(uint16_t dst, uint8_t ep, uint16_t seconds) { (void)dst; (void)ep; (void)seconds; return ESP_OK; }
esp_err_t zb_setup_reports(uint16_t dst, uint8_t ep) { (void)dst; (void)ep; return ESP_OK; }
void zlink_discover(uint16_t addr) { (void)addr; }
static int g_reint[16], g_nreint;   // instances the link was asked to interview again and set up anew
bool zlink_reinterview(int inst)
{
    if (g_nreint < 16) g_reint[g_nreint++] = inst;
    return inst >= 100 && inst < 100 + KNX_VDEV_MAX && g_d[inst - 100].used;
}
static int g_note_kind, g_nnote;
static uint16_t g_note_ia;
void gw_note(int kind, uint16_t ia, uint8_t err, const char *why) { (void)err; (void)why; g_note_kind = kind; g_note_ia = ia; g_nnote++; }
// task watchdog (main.c): the pair task subscribes when it starts and feeds with every round
static unsigned long g_wdt_watch, g_wdt_feed;
void wdt_watch(void) { g_wdt_watch++; }
void wdt_feed(void) { g_wdt_feed++; }

// ---- the checks
static int g_pass, g_fail;
static void check(const char *what, bool cond)
{
    printf("  %s %s\n", cond ? "ok  " : "FAIL", what);
    if (cond) g_pass++; else g_fail++;
}
static const uint8_t IEEE_A[8] = {1, 2, 3, 4, 5, 6, 7, 0xA}, IEEE_B[8] = {1, 2, 3, 4, 5, 6, 7, 0xB};
static void reset(void)   // devices and records as at the start; the flash keeps what it has
{
    memset(g_d, 0, sizeof g_d);
    g_ndel = g_nleave = g_nnote = g_nopen = 0;
    g_note_kind = 0;
    g_open_s = -1;
    s_st = ST_IDLE;
    s_slot = s_replace = -1;
    s_armed = s_have_cand = false;
    s_closed = 0;
    memset(s_qs, 0, sizeof s_qs);
    memset(s_born, 0, sizeof s_born);
    memset(s_unload, 0, sizeof s_unload);
    memset(s_cap_known, 0, sizeof s_cap_known);
    memset(s_pend_t, 0, sizeof s_pend_t);
    run();
}
static int dev(uint16_t ia, uint16_t zaddr, const uint8_t *ieee)
{
    int i = vdev_add(zaddr, 1, -1, ieee);
    g_d[i].ia = ia;
    return i;
}
static void unload(int i, uint16_t old)   // as vdev.c: 15.15.255 is written, pair.c is told at once
{
    g_d[i].ia = 0xFFFF;
    pair_knx_unloaded(i, old);
    run();
}
static void ask(uint16_t src, uint16_t target, int own)   // ETS programs an address: its question and two reads
{
    pair_knx_probe(src, target, own);
    pair_knx_poll(src);
    run();
    pair_knx_poll(src);
    run();
}
static void asking(uint16_t src, int ms)   // ... and goes on reading every 3 s
{
    while (ms > 0) {
        int step = ms > 3000 ? 3000 : ms;
        pair_knx_poll(src);
        later(step);
        ms -= step;
    }
}
static void fresh(uint16_t sa, const uint8_t *ieee)   // a device joins anew: Device_Update "joined", then its announce
{
    pair_zb_update(sa, ieee, 1);
    pair_zb_announce(sa, ieee, 0x8e);
    pair_zb_authorized(sa, true);
}

int main(int argc, char **argv)
{
    g_verbose = argc > 1 && strcmp(argv[1], "-v") == 0;
    pair_start();
    check("the default range is 15.15.200 .. 15.15.231", s_lo == 0xFFC8 && s_hi == 0xFFE7);
    pair_range(0xFF01, 0xFF30);   // the range of these tests, as a download of the gateway sets it: 15.15.1 .. 15.15.48
    check("the settings are kept in the device server's partition of this board", strcmp(g_part, BOARD_DEVSRV_PART) == 0);

    printf("the task watchdog\n");
    unsigned long f0 = g_wdt_feed;
    later(1000);
    later(1000);
    check("the pair task subscribes and feeds it with every round, also when no event comes", g_wdt_watch > 0 &&
          g_wdt_feed >= f0 + 2);

    printf("a gateway whose own application ETS has not loaded yet\n");
    reset();
    ask(0x1101, 0xFF05, -1);
    asking(0x1101, 6000);
    check("an address assignment in the range opens nothing", g_nopen == 0 && s_st == ST_IDLE);
    pair_window(60);
    run();
    check("the pairing window opens all the same", s_st == ST_WINDOW && g_nopen == 1);
    pair_zb_update(0x7d00, IEEE_A, 1);
    pair_zb_authorized(0x7d00, true);
    run();
    int w = vdev_by_ieee(IEEE_A);
    check("... and the device that joins through it waits without an address", w >= 0 && vdev_unbound(w) &&
          s_st == ST_IDLE);
    later(8000);
    ask(0x1101, 0xFF06, -1);
    asking(0x1101, 6000);
    check("ETS asks for an address while it waits: nothing, the device is not offered", s_st == ST_IDLE && !g_d[w].prog &&
          g_nopen == 1);
    pair_gw_loaded();
    later(8000);
    ask(0x1101, 0xFF06, -1);
    check("the gateway is loaded: the same question takes the device that waits", s_st == ST_CHECK && s_slot == w);

    printf("ETS unloads the address of a device that is on\n");
    reset();
    int a = dev(0xFF02, 0x1234, IEEE_A);
    unload(a, 0xFF02);
    check("nothing goes at once: ETS may still talk to the device", g_ndel == 0 && g_nleave == 0);
    later(4000);
    check("... nor after 4 s", g_ndel == 0 && g_nleave == 0);
    later(2000);
    check("after 6 s the virtual device is removed", g_ndel == 1 && g_dels[0] == a && !g_d[a].used);
    check("its Zigbee device is asked to leave, once", g_nleave == 1 && g_leave[0] == 0x1234);
    check("the gateway reports the removal with the address the device had", g_nnote == 1 &&
          g_note_kind == GW_NOTE_REMOVED && g_note_ia == 0xFF02);
    check("it is remembered until it is seen leaving", s_pend_n == 1 && pend_find(IEEE_A) == 0);
    pair_zb_left(0x1234, IEEE_A);
    run();
    check("its leave indication ends that", s_pend_n == 0 && kv("leave", false) < 0);
    pair_zb_announce(0x1234, IEEE_A, 0x8e);
    run();
    check("a later announce of a device that left gets no request", g_nleave == 1);

    printf("ETS unloads an address and gives another one right away\n");
    reset();
    a = dev(0xFF02, 0x1234, IEEE_A);
    unload(a, 0xFF02);
    later(2000);
    g_d[a].ia = 0xFF07;
    pair_knx_assigned(a, 0xFF07);
    later(10000);
    check("the device stays, nobody is asked to leave, nothing is reported", g_ndel == 0 && g_nleave == 0 &&
          g_nnote == 0 && g_d[a].used);

    printf("the device was switched off when ETS unloaded its address\n");
    reset();
    a = dev(0xFF03, 0x2000, IEEE_B);
    unload(a, 0xFF03);
    later(6000);
    check("removed and asked to leave (the request reaches nobody)", g_ndel == 1 && g_nleave == 1 && s_pend_n == 1);
    later(3600 * 1000);
    check("an hour later it is still remembered", s_pend_n == 1 && g_nleave == 1);
    pair_zb_update(0x2001, IEEE_B, 0);   // it comes back as the member it was: secured rejoin, another short address
    pair_zb_announce(0x2001, IEEE_B, 0x8e);
    run();
    check("back on the network under another short address: asked to leave there", g_nleave == 2 && g_leave[1] == 0x2001);
    check("no virtual device is made for it and the network stays closed", vdev_free_slots() == KNX_VDEV_MAX && g_nopen == 0);
    pair_zb_announce(0x2001, IEEE_B, 0x8e);
    run();
    check("its announce right after the rejoin gets no second request", g_nleave == 2);
    later(15000);
    pair_zb_announce(0x2001, IEEE_B, 0x8e);
    run();
    check("it announces again later, before it left: asked again", g_nleave == 3);
    pair_zb_left(0x2001, IEEE_B);
    run();
    check("its leave indication ends that", s_pend_n == 0);

    printf("what is remembered survives a restart\n");
    reset();
    a = dev(0xFF04, 0x3000, IEEE_A);
    unload(a, 0xFF04);
    later(6000);
    check("written to the flash", kv("leave", false) >= 0 && g_kv[kv("leave", false)].len == 8);
    s_pend_n = 0;   // what the RAM knew is gone with the restart
    memset(s_pend, 0, sizeof s_pend);
    memset((void *)s_pend_sa, 0, sizeof s_pend_sa);
    pend_load();
    check("read back at the start", s_pend_n == 1 && pend_find(IEEE_A) == 0);
    int before = g_nleave;
    pair_zb_announce(0x3000, IEEE_A, 0x8e);
    run();
    check("the device that comes back after the restart is asked to leave", g_nleave == before + 1);

    printf("a device that was removed is paired again\n");
    // still remembered from the case above; it was reset by hand and joins anew while ETS asks for an address
    reset();
    check("still remembered", pend_find(IEEE_A) == 0);
    pair_knx_probe(0x1101, 0xFF05, -1);
    pair_knx_poll(0x1101);
    run();
    pair_knx_poll(0x1101);
    run();
    check("ETS asks for an address in the range: the network opens", g_nopen == 1 && s_st == ST_OPEN);
    before = g_nleave;
    fresh(0x3100, IEEE_A);
    run();
    check("the new join is no longer the member that was to leave", s_pend_n == 0);
    check("it becomes a virtual device in programming mode and is not asked to leave", g_nleave == before &&
          vdev_by_ieee(IEEE_A) >= 0 && g_d[vdev_by_ieee(IEEE_A)].prog);

    printf("the console removes a device (vdev del) whose request to leave got lost\n");
    reset();
    pair_removed(IEEE_B);
    run();
    check("remembered", pend_find(IEEE_B) >= 0);
    before = g_nleave;
    pair_zb_announce(0x4000, IEEE_B, 0x8e);
    run();
    check("asked to leave when it shows up", g_nleave == before + 1 && g_leave[g_nleave - 1] == 0x4000);
    pair_zb_left(0x4000, IEEE_B);
    run();

    printf("a member nobody remembers\n");
    reset();
    static const uint8_t other[8] = {9, 9, 9, 9, 9, 9, 9, 9};
    before = g_nleave;
    pair_zb_announce(0x5000, other, 0x8e);
    run();
    check("a member without a virtual device that was never removed here is left alone", g_nleave == before);
    a = dev(0xFF06, 0x5100, IEEE_A);
    pair_zb_announce(0x5100, IEEE_A, 0x8e);
    run();
    check("one of ours that announces itself is left alone", g_nleave == before && g_d[a].used);

    printf("unloading where there is nothing to ask\n");
    reset();
    a = dev(0xFF08, 0x6000, IEEE_A);
    vdev_gone(a);
    unload(a, 0xFF08);
    later(6000);
    check("the Zigbee device had left already: removed, nobody asked, nothing remembered", g_ndel == 1 && g_nleave == 0 && s_pend_n == 0);
    reset();
    a = dev(0xFF09, 0xFFFF, NULL);   // a virtual device without a Zigbee device (console: vdev add none)
    unload(a, 0xFF09);
    later(6000);
    check("no Zigbee device behind it: removed, nobody asked", g_ndel == 1 && g_nleave == 0 && g_nnote == 1);
    reset();
    a = dev(0xFF0A, 0x6100, NULL);   // linked at the console by its short address: no IEEE address known
    unload(a, 0xFF0A);
    later(6000);
    check("IEEE address not known: asked to leave once, nothing to remember", g_ndel == 1 && g_nleave == 1 && s_pend_n == 0);

    printf("unloading the device a series holds in programming mode\n");
    reset();
    a = dev(0xFF0B, 0x7000, IEEE_A);
    pair_knx_probe(0x1101, 0xFF0B, a);
    pair_knx_poll(0x1101);
    run();
    pair_knx_poll(0x1101);
    run();
    check("ETS asks for the device's own address: it goes into programming mode", s_st == ST_PENDING && g_d[a].prog);
    unload(a, 0xFF0B);
    for (int i = 0; i < 3; i++) {   // ETS goes on reading for a moment
        pair_knx_poll(0x1101);
        later(2000);
    }
    check("removed after the delay, the series is over and holds no device", g_ndel == 1 && s_st == ST_IDLE && s_slot == -1);
    later(20000);
    check("nothing else happens afterwards", g_ndel == 1 && g_nleave == 1 && g_nopen == 0);
    pair_zb_left(0x7000, IEEE_A);
    run();

    printf("a device that waits for its address\n");
    reset();
    a = dev(0xFFFF, 0x8000, IEEE_B);
    unload(a, 0xFFFF);   // 15.15.255 written over 15.15.255
    later(6000);
    check("an address unloaded where there was none removes nothing", g_ndel == 0 && g_nleave == 0 && g_nnote == 0);
    later(31 * 60 * 1000);
    check("but it does not wait for ever: removed after half an hour, asked to leave, remembered", g_ndel == 1 &&
          g_nleave == 1 && pend_find(IEEE_B) >= 0);
    pair_zb_left(0x8000, IEEE_B);
    run();
    reset();
    a = dev(0xFFFF, 0x8100, IEEE_B);   // it waited when the gateway restarted
    memset(s_born, 0, sizeof s_born);
    pair_start();   // the queue is kept: Zigbee may have filled it before
    later(29 * 60 * 1000);
    check("after a restart a waiting device has its half hour again", g_ndel == 0);
    later(2 * 60 * 1000);
    check("... and no more", g_ndel == 1 && g_nleave == 1);
    pair_zb_left(0x8100, IEEE_B);
    run();

    printf("ETS finds a waiting device silent\n");
    reset();
    a = dev(0xFFFF, 0x9000, IEEE_A);
    pair_knx_probe(0x1101, 0xFF0C, -1);
    pair_knx_poll(0x1101);
    run();
    pair_knx_poll(0x1101);
    run();
    check("it is asked whether it is still there", s_st == ST_CHECK);
    pair_knx_poll(0x1101);
    later(3500);
    check("no answer: removed, asked to leave, remembered", g_ndel == 1 && g_nleave == 1 && pend_find(IEEE_A) >= 0);
    pair_knx_poll(0x1101);
    later(1000);
    check("the same series opens the network with ETS's next read", s_st == ST_OPEN && g_nopen == 1);
    pair_zb_left(0x9000, IEEE_A);
    run();

    printf("a device joins and the stack reports no announce (seen 2026-11-15)\n");
    reset();
    ask(0x1101, 0xFF10, -1);
    check("the network is open for the address ETS asks for", s_st == ST_OPEN && g_open_s == OPEN_S);
    pair_zb_update(0x6f0b, IEEE_A, 1);
    run();
    check("the join alone makes it the candidate and closes the network", s_st == ST_AUTH && g_open_s == 0);
    asking(0x1101, 11900);
    check("11.9 s later it still waits for its key exchange, nobody is asked to leave", s_st == ST_AUTH && g_nleave == 0);
    pair_zb_authorized(0x6f0b, true);
    run();
    int v = vdev_by_ieee(IEEE_A);
    check("the key exchange ends: a virtual device in programming mode, without any announce", s_st == ST_PENDING &&
          v >= 0 && g_d[v].prog && g_d[v].zaddr == 0x6f0b);
    pair_zb_announce(0x6f0b, IEEE_A, 0x8e);
    run();
    check("an announce that comes after all changes nothing", s_st == ST_PENDING && g_nleave == 0 && g_ndel == 0);

    printf("the key exchange never ends\n");
    reset();
    ask(0x1101, 0xFF11, -1);
    pair_zb_update(0x7001, IEEE_B, 1);
    run();
    asking(0x1101, 29000);
    check("29 s: still the candidate", s_st == ST_AUTH && g_nleave == 0);
    asking(0x1101, 2500);
    check("after 30 s it is asked to leave and remembered", g_nleave == 1 && g_leave[0] == 0x7001 && pend_find(IEEE_B) >= 0);
    asking(0x1101, 3000);
    check("the series opens the network again with ETS's next read", s_st == ST_OPEN && g_nopen == 2);
    pair_zb_announce(0x7001, IEEE_B, 0x8e);   // it is still a member and shows up
    later(15000);
    pair_zb_announce(0x7001, IEEE_B, 0x8e);
    run();
    check("as long as it is a member it is asked again when it shows up", g_nleave >= 2);
    pair_zb_left(0x7001, IEEE_B);
    run();
    check("seen leaving: forgotten", pend_find(IEEE_B) < 0);

    printf("the stack says the key exchange failed\n");
    reset();
    ask(0x1101, 0xFF11, -1);
    pair_zb_update(0x7002, IEEE_B, 1);
    pair_zb_authorized(0x7002, false);
    run();
    check("asked to leave at once and remembered, no virtual device", g_nleave == 1 && pend_find(IEEE_B) >= 0 &&
          vdev_by_ieee(IEEE_B) < 0 && s_st == ST_IDLE);
    pair_zb_update(0x7002, IEEE_B, 2);
    run();
    check("Device_Update \"left\" ends the remembering as a leave indication does", pend_find(IEEE_B) < 0);

    printf("the network is opened from elsewhere (seen 2026-11-15)\n");
    reset();
    ask(0x1101, 0xFF12, -1);
    pair_zb_opened(180);
    run();
    check("while a series holds it open it stays open: state and network agree", s_st == ST_OPEN && g_open_s == OPEN_S &&
          pair_is_open());
    pair_zb_update(0x7100, IEEE_A, 1);
    pair_zb_authorized(0x7100, true);
    run();
    check("a device that joins afterwards is bound", s_st == ST_PENDING && vdev_by_ieee(IEEE_A) >= 0);
    g_open_s = 99;
    pair_zb_opened(180);
    run();
    check("with a device in programming mode it is closed, as before", g_open_s == 0 && s_st == ST_PENDING);
    reset();
    pair_window(60);
    run();
    pair_zb_opened(180);
    run();
    check("the pairing window stays open as well", s_st == ST_WINDOW && g_open_s == 60);
    later(61000);
    check("... and closes at its own end", s_st == ST_IDLE && g_open_s == 0);
    g_open_s = 99;
    pair_zb_opened(180);
    run();
    check("with nothing under way it is closed", g_open_s == 0);

    printf("ETS unloads a device and programs an address right away\n");
    reset();
    a = dev(0xFF13, 0x7200, IEEE_A);
    unload(a, 0xFF13);
    later(1000);
    ask(0x1101, 0xFF13, -1);
    check("the unloaded device is not offered as one that waits: the network opens", s_st == ST_OPEN && s_slot == -1);
    asking(0x1101, 6000);
    check("it is removed after its 5 s and the series goes on", g_ndel == 1 && g_dels[0] == a && s_st == ST_OPEN);
    remove_device(-1, "no such device");
    remove_device(KNX_VDEV_MAX, "no such device");
    check("removing a device that is none does nothing", g_ndel == 1);
    pair_zb_left(0x7200, IEEE_A);
    run();

    printf("the gateway restarts between the unloading and the removal\n");
    reset();
    a = dev(0xFF14, 0x7300, IEEE_B);
    unload(a, 0xFF14);
    check("the unloading is in the flash", kv("unload", false) >= 0);
    later(2000);
    memset(s_unload, 0, sizeof s_unload);
    memset(s_born, 0, sizeof s_born);
    pair_start();
    check("after the restart it is not a device that waits for an address", s_born[a] == 0 && s_unload[a] != 0);
    later(1000);
    check("it is removed with the first tick and reported with the address it had", g_ndel == 1 &&
          g_note_kind == GW_NOTE_REMOVED && g_note_ia == 0xFF14);
    check("nothing is left in the flash", kv("unload", false) < 0);
    pair_zb_left(0x7300, IEEE_B);
    run();

    printf("the device leaves by itself within the 5 s\n");
    reset();
    a = dev(0xFF19, 0x7700, IEEE_A);
    unload(a, 0xFF19);
    pair_zb_left(0x7700, IEEE_A);
    run();
    check("removed at once and reported like the removal that was coming", g_ndel == 1 && g_nnote == 1 &&
          g_note_ia == 0xFF19 && s_unload[a] == 0);
    int b2 = dev(0xFFFF, 0x7701, IEEE_B);   // the next device gets the same place
    later(6000);
    check("the device that takes its place is not removed in its stead", b2 == a && g_ndel == 1 && g_d[b2].used);

    printf("two addresses one after the other\n");
    reset();
    ask(0x1101, 0xFF15, -1);
    fresh(0x7400, IEEE_A);
    run();
    v = vdev_by_ieee(IEEE_A);
    g_d[v].ia = 0xFF15;
    pair_knx_assigned(v, 0xFF15);
    run();
    check("the first is bound", s_st == ST_DONE);
    pair_knx_probe(0x1101, 0xFF15, v);   // ETS connects to the device it just addressed and reads once more
    pair_knx_poll(0x1101);
    run();
    check("ETS connecting to that device changes nothing", s_st == ST_DONE && g_nopen == 1);
    later(3000);
    ask(0x1101, 0xFF16, -1);
    check("the next assignment 3 s later opens the network", s_st == ST_OPEN && g_nopen == 2 && !g_d[v].prog);

    printf("a device behind a router leaves: only Device_Update says so\n");
    reset();
    a = dev(0xFF17, 0x7500, IEEE_A);
    pair_zb_update(0x7500, IEEE_A, 2);
    run();
    check("its virtual device keeps everything and is marked as gone", g_d[a].used && g_d[a].gone && g_ndel == 0);

    printf("a second device joins in the same opening\n");
    reset();
    ask(0x1101, 0xFF18, -1);
    pair_zb_update(0x7600, IEEE_A, 1);
    pair_zb_update(0x7601, IEEE_B, 1);
    pair_zb_announce(0x7601, IEEE_B, 0x8e);
    run();
    check("it is asked to leave once and remembered", g_nleave == 1 && g_leave[0] == 0x7601 && pend_find(IEEE_B) >= 0);
    pair_zb_left(0x7601, IEEE_B);
    pair_zb_authorized(0x7600, true);
    run();
    check("the first becomes the virtual device", s_st == ST_PENDING && vdev_by_ieee(IEEE_A) >= 0 && pend_find(IEEE_B) < 0);

    printf("ETS reads for 4 min and writes no address\n");
    reset();
    ask(0x1101, 0xFF1A, -1);
    fresh(0x7900, IEEE_A);
    run();
    v = vdev_by_ieee(IEEE_A);
    check("a device is in programming mode for ETS", s_st == ST_PENDING && v >= 0 && g_d[v].prog);
    asking(0x1101, 239000);
    check("ETS goes on reading: the series holds", s_st == ST_PENDING);
    pair_knx_prog_ended(v + 1);
    run();
    check("the programming mode of another device ends: nothing to do with this series", s_st == ST_PENDING);
    pair_knx_prog_ended(v);
    run();
    check("the device server ends the programming mode after 4 min: the series is over", s_st == ST_IDLE &&
          s_slot == -1 && !g_d[v].prog);
    asking(0x1101, 9000);
    check("the reads that follow open nothing", s_st == ST_IDLE && g_nopen == 1);

    printf("a device nobody gave a time\n");
    reset();
    a = dev(0xFFFF, 0x7800, IEEE_B);   // without an address and pair.c was never told (an event lost)
    later(11000);                      // the look that finds it; the clock of this test jumps, the firmware ticks
    later(29 * 60 * 1000);
    check("it has its half hour from the look that found it", g_ndel == 0);
    later(2 * 60 * 1000);
    check("... and is removed after it", g_ndel == 1 && g_nleave == 1);
    pair_zb_left(0x7800, IEEE_B);
    run();

    printf("two sources ask for an address at the same time\n");
    reset();
    pair_knx_probe(0x1101, 0xFF20, -1);   // the first ETS
    pair_knx_probe(0x1102, 0xFF21, -1);   // a second one, a moment later
    pair_knx_poll(0x1101);
    pair_knx_poll(0x1102);
    run();
    pair_knx_poll(0x1101);
    pair_knx_poll(0x1102);
    run();
    check("the first source is served although another one asked after it", s_st == ST_OPEN && s_src == 0x1101 &&
          s_target == 0xFF20 && g_nopen == 1);
    for (int i = 0; i < 3; i++) {   // the first gives up, the second reads on every 3 s
        pair_knx_poll(0x1102);
        later(3000);
    }
    check("the first gave up: its series is over and the network closed", s_st == ST_IDLE && g_open_s == 0);
    for (int i = 0; i < 2; i++) {
        pair_knx_poll(0x1102);
        later(3000);
    }
    check("the second, still reading, is served then: the network opens for its address", s_st == ST_OPEN &&
          s_src == 0x1102 && s_target == 0xFF21 && g_nopen == 2);
    later(8000);
    check("... until it gives up as well", s_st == ST_IDLE && g_open_s == 0);
    pair_knx_probe(0x1101, 0xFF20, -1);   // the first asks again, and a third source asks meanwhile and reads once
    pair_knx_probe(0x1103, 0xFF22, -1);
    pair_knx_poll(0x1101);
    pair_knx_poll(0x1103);
    run();
    pair_knx_poll(0x1101);
    run();
    check("the first is served again", s_st == ST_OPEN && s_src == 0x1101 && g_nopen == 3);
    for (int i = 0; i < 3; i++) {   // only the first reads on
        pair_knx_poll(0x1101);
        later(3000);
    }
    later(8000);
    check("the first gives up", s_st == ST_IDLE);
    pair_knx_poll(0x1103);
    run();
    pair_knx_poll(0x1103);
    run();
    check("a question whose reads stopped for longer than ETS waits between two opens nothing when its turn comes",
          s_st == ST_IDLE && g_nopen == 3);
    pair_knx_probe(0x1104, 0xFF23, -1);
    run();
    later(2500);   // the first read comes too late after the question
    pair_knx_poll(0x1104);
    run();
    pair_knx_poll(0x1104);
    run();
    check("a question counts only if its first read follows within 2 s", s_st == ST_IDLE && g_nopen == 3);

    printf("a sleepy device came through the pairing window and waits; ETS asks for an address\n");
    reset();
    pair_window(60);
    run();
    pair_zb_update(0x7a00, IEEE_A, 1);
    pair_zb_announce(0x7a00, IEEE_A, 0x80);   // receiver off when idle: a remote
    pair_zb_authorized(0x7a00, true);
    run();
    int r = vdev_by_ieee(IEEE_A);
    check("it waits without an address", r >= 0 && vdev_unbound(r) && s_st == ST_IDLE);
    later(5 * 60 * 1000);
    int reads = g_nread;
    ask(0x1101, 0xFF24, -1);
    check("it is not asked whether it is there: programming mode at once", s_st == ST_PENDING && s_slot == r &&
          g_d[r].prog && g_nread == reads);
    asking(0x1101, 9000);
    check("nobody is removed or asked to leave while ETS reads", g_d[r].used && g_ndel == 0 && g_nleave == 0);
    g_d[r].ia = 0xFF24;
    pair_knx_assigned(r, 0xFF24);
    run();
    check("ETS gives it the address", s_st == ST_DONE && !vdev_unbound(r));
    reset();
    pair_window(60);
    run();
    pair_zb_update(0x7a01, IEEE_B, 1);
    pair_zb_authorized(0x7a01, true);   // its key exchange is through before its announce
    run();
    pair_zb_announce(0x7a01, IEEE_B, 0x80);
    run();
    later(60000);
    r = vdev_by_ieee(IEEE_B);
    ask(0x1101, 0xFF25, -1);
    check("an announce after the key exchange tells it as well", r >= 0 && s_st == ST_PENDING && s_slot == r);
    reset();
    pair_window(60);
    run();
    pair_zb_update(0x7a02, IEEE_A, 1);
    pair_zb_announce(0x7a02, IEEE_A, 0x8e);   // receiver on: a lamp
    pair_zb_authorized(0x7a02, true);
    run();
    later(60000);
    ask(0x1101, 0xFF26, -1);
    check("a waiting lamp is still asked whether it is there", s_st == ST_CHECK);
    later(4000);
    check("... and removed when it does not answer", g_ndel == 1);
    pair_zb_left(0x7a02, IEEE_A);
    run();

    printf("a device asked to leave goes on sending reports\n");
    reset();
    a = dev(0xFF27, 0x7b00, IEEE_A);
    unload(a, 0xFF27);
    later(6000);
    check("removed and asked to leave once", g_ndel == 1 && g_nleave == 1 && pend_find(IEEE_A) >= 0);
    pair_zb_heard(0x7b00);
    run();
    check("a report right after the request: not asked again within 10 s", g_nleave == 1);
    later(11000);
    pair_zb_heard(0x7b00);
    run();
    check("a report later from the address it was asked at: asked again", g_nleave == 2 && g_leave[1] == 0x7b00);
    pair_zb_heard(0x7b00);
    pair_zb_heard(0x7b01);   // somebody else
    run();
    check("... not twice within 10 s, and nobody else is asked", g_nleave == 2);
    later(11000);
    s_pend_n = 0;   // a restart: the RAM forgets, the flash keeps the short address with the IEEE address
    memset(s_pend, 0, sizeof s_pend);
    memset((void *)s_pend_sa, 0, sizeof s_pend_sa);
    pend_load();
    pair_zb_heard(0x7b00);
    run();
    check("after a restart its report still gets it the request", g_nleave == 3 && g_leave[2] == 0x7b00);
    later(11000);
    int b = dev(0xFF28, 0x7b00, IEEE_B);   // it left unseen, and one of our devices has that short address now
    pair_zb_heard(0x7b00);
    run();
    check("a frame from that address when one of our devices has it asks nobody", g_nleave == 3 && g_d[b].used);
    vdev_del(b);
    later(11000);
    pair_zb_heard(0x7b00);
    run();
    check("... and the address no longer stands for the device that was asked", g_nleave == 3 && pend_find(IEEE_A) >= 0);
    pair_zb_announce(0x7b05, IEEE_A, 0x8e);   // it shows up under another short address: asked there
    run();
    check("its announce under another address gets it the request there", g_nleave == 4 && g_leave[3] == 0x7b05);
    later(11000);
    static const uint8_t IEEE_C[8] = {1, 2, 3, 4, 5, 6, 7, 0xC};
    pair_zb_update(0x7b05, IEEE_C, 1);   // a new device joins with that short address (the network is closed)
    run();
    int pa = pend_find(IEEE_A);
    check("a device that joins with that address while nothing is open is asked to leave; the one asked before no "
          "longer stands for the address", g_nleave == 5 && g_leave[4] == 0x7b05 && pend_find(IEEE_C) >= 0 && pa >= 0 &&
          s_pend_sa[pa] == SA_NONE);
    int64_t asked_a = s_pend_t[pa];
    later(11000);
    pair_zb_heard(0x7b05);
    run();
    check("a frame from that address asks the device that joined there once more, not the one asked before",
          g_nleave == 6 && s_pend_t[pend_find(IEEE_A)] == asked_a);
    pair_zb_left(0x7b05, IEEE_A);
    pair_zb_left(0x7b05, IEEE_C);
    run();

    printf("a device joins while nothing here holds the network open\n");
    reset();
    later(60000);
    pair_zb_update(0x7e00, IEEE_A, 1);
    pair_zb_announce(0x7e00, IEEE_A, 0x8e);
    run();
    check("a minute after the start, in idle: asked to leave, remembered, warned", g_nleave == 1 && g_leave[0] == 0x7e00 &&
          pend_find(IEEE_A) >= 0 && strstr(g_lastw, "joined while nothing was open") != NULL);
    check("... no virtual device for it, and the network is closed once more", vdev_by_ieee(IEEE_A) < 0 &&
          g_open_s == 0 && s_st == ST_IDLE);
    pair_zb_left(0x7e00, IEEE_A);
    run();
    reset();
    ask(0x1101, 0xFF2C, -1);
    fresh(0x7e10, IEEE_A);
    run();
    int v1 = vdev_by_ieee(IEEE_A);
    check("a series binds a device", v1 >= 0);
    if (v1 < 0) return 1;
    g_d[v1].ia = 0xFF2C;
    pair_knx_assigned(v1, 0xFF2C);
    run();
    check("... and its address is written", s_st == ST_DONE && g_nleave == 0);
    later(20000);
    g_open_s = 99;
    pair_zb_update(0x7e11, IEEE_B, 1);
    pair_zb_announce(0x7e11, IEEE_B, 0x8e);
    run();
    check("a second device joins 20 s later: asked to leave, the network closed once more", g_nleave == 1 &&
          g_leave[0] == 0x7e11 && g_open_s == 0 && vdev_by_ieee(IEEE_B) < 0);
    pair_zb_left(0x7e11, IEEE_B);
    run();
    reset();
    later(60000);
    pair_manual(60);
    run();
    pair_zb_update(0x7e20, IEEE_B, 1);
    run();
    check("the console's \"join\" holds the network open: whoever joins stays", g_nleave == 0 && pend_find(IEEE_B) < 0);
    later(61000);
    pair_zb_update(0x7e21, IEEE_A, 1);
    run();
    check("... and after its seconds the next one is asked to leave", g_nleave == 1 && g_leave[0] == 0x7e21);
    pair_manual(60);
    run();
    later(16000);     // beyond the guard after the last closing
    pair_manual(0);   // "join 0" closes it before its time
    run();
    pair_zb_update(0x7e22, IEEE_C, 1);
    run();
    check("\"join 0\" ends it before its time", g_nleave == 2 && g_leave[1] == 0x7e22 &&
          strstr(g_lastw, "joined while nothing was open") != NULL);
    pair_zb_left(0x7e21, IEEE_A);
    pair_zb_left(0x7e22, IEEE_C);
    run();

    printf("one of our devices joins anew after a reset by hand\n");
    reset();
    a = dev(0xFF29, 0x7c00, IEEE_A);
    ask(0x1101, 0xFF2A, -1);
    check("ETS waits for a new device for another address: the network is open", s_st == ST_OPEN);
    int nre = g_nreint;
    pair_zb_update(0x7c00, IEEE_A, 1);   // joined, not a rejoin; with its old short address
    pair_zb_announce(0x7c00, IEEE_A, 0x8e);
    pair_zb_authorized(0x7c00, true);
    run();
    check("it stays with its virtual device: no new one, nobody asked to leave",
          vdev_free_slots() == KNX_VDEV_MAX - 1 && g_d[a].used && g_nleave == 0);
    check("it is set up again: binding, reporting, the parameters of its class", g_nreint == nre + 1 &&
          g_reint[nre] == 100 + a);
    check("the console says what happened", strstr(g_lastw, "joined anew") != NULL &&
          strstr(g_lastw, "not the new device") != NULL);
    check("the network stays open for the device ETS waits for", s_st == ST_OPEN && g_open_s == OPEN_S);
    pair_zb_update(0x7c00, IEEE_A, 0);   // a secured rejoin is no reset
    pair_zb_announce(0x7c00, IEEE_A, 0x8e);
    run();
    check("a rejoin sets nothing up anew", g_nreint == nre + 1);

    printf("more devices to remember than there is room for\n");
    reset();
    for (int i = 0; i < PEND_MAX + 2; i++) {
        uint8_t ieee[8] = {0x10, 0, 0, 0, 0, 0, 0, (uint8_t)i};
        pair_removed(ieee);
        run();
    }
    uint8_t first[8] = {0x10, 0, 0, 0, 0, 0, 0, 0}, third[8] = {0x10, 0, 0, 0, 0, 0, 0, 2},
            last[8] = {0x10, 0, 0, 0, 0, 0, 0, PEND_MAX + 1};
    check("the list holds its number, the oldest two went", s_pend_n == PEND_MAX && pend_find(first) < 0 &&
          pend_find(third) == 0 && pend_find(last) == PEND_MAX - 1);
    static const uint8_t zero[8];
    pair_removed(zero);
    pair_removed(last);
    run();
    check("no address and one that is there already add nothing", s_pend_n == PEND_MAX);
    pair_zb_left(0x1111, last);
    run();
    check("the newest of a full list is seen leaving: the others stay in their order", s_pend_n == PEND_MAX - 1 &&
          pend_find(last) < 0 && pend_find(third) == 0);
    s_pend_n = 0;
    memset(s_pend, 0, sizeof s_pend);
    pend_load();
    check("... and that is what the flash holds", s_pend_n == PEND_MAX - 1 && pend_find(third) == 0);

    free(s_q->buf);
    free(s_q);
    printf("pair checks: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
