// Host test of main/zlink.c: availability decisions turned into actions and the full interview, with the real link
// task on a virtual clock and a queue that drops when full (as xQueueSend(..., 0)).
// Ends with "zlink checks: N passed, M failed".
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zlink.c"

static int64_t g_now = 1000000000LL;
int64_t esp_timer_get_time(void) { return g_now; }
#define NT 160
static struct stub_timer g_t[NT];
static int g_nt;
esp_err_t esp_timer_create(const esp_timer_create_args_t *a, esp_timer_handle_t *h)
{
    struct stub_timer *t = &g_t[g_nt++];
    t->cb = a->callback; t->arg = a->arg; t->name = a->name; t->armed = false;
    *h = t;
    return ESP_OK;
}
esp_err_t esp_timer_start_once(esp_timer_handle_t h, uint64_t us)
{
    if (h->armed) return 0x103;
    h->armed = true; h->due = g_now + (int64_t)us; h->period = 0;
    return ESP_OK;
}
esp_err_t esp_timer_start_periodic(esp_timer_handle_t h, uint64_t us)
{
    h->armed = true; h->due = g_now + (int64_t)us; h->period = us;
    return ESP_OK;
}
esp_err_t esp_timer_stop(esp_timer_handle_t h)
{
    if (!h->armed) return 0x103;
    h->armed = false;
    return ESP_OK;
}
struct stub_queue { int n, size, head, count, drops; unsigned char *buf; };
QueueHandle_t xQueueCreate(UBaseType_t n, UBaseType_t size)
{
    struct stub_queue *q = calloc(1, sizeof *q);
    q->n = (int)n; q->size = (int)size; q->buf = calloc(n, size);
    return q;
}
BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t t)
{
    (void)t;
    if (q->count == q->n) { q->drops++; return pdFALSE; }
    memcpy(q->buf + (size_t)((q->head + q->count) % q->n) * q->size, item, (size_t)q->size);
    q->count++;
    return pdTRUE;
}
static jmp_buf g_jb;
static int g_in_task;
BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t t)
{
    (void)t;
    if (!q->count) {
        if (g_in_task) longjmp(g_jb, 1);
        return pdFALSE;
    }
    memcpy(item, q->buf + (size_t)q->head * q->size, (size_t)q->size);
    q->head = (q->head + 1) % q->n;
    q->count--;
    return pdTRUE;
}
struct stub_sem { int taken; };
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return calloc(1, sizeof(struct stub_sem)); }
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t t)
{
    (void)t;
    if (s->taken) { printf("DEADLOCK: a mutex taken twice by the same thread\n"); abort(); }
    s->taken = 1;
    return pdTRUE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t s) { s->taken = 0; return pdTRUE; }
BaseType_t xTaskCreate(void (*fn)(void *), const char *name, uint32_t stack, void *arg, UBaseType_t prio, TaskHandle_t *h)
{
    (void)fn; (void)name; (void)stack; (void)arg; (void)prio; (void)h;
    return pdTRUE;   // link_task runs in drain()
}
static int g_verbose = 1;
static char g_dropped[256];
void stub_log(char lvl, const char *tag, const char *fmt, ...)
{
    char b[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (strstr(b, "queue full")) {
        char *p = strstr(b, "full, ");
        if (p) { strncat(g_dropped, p + 6, 24); strcat(g_dropped, " | "); }
    }
    if (g_verbose) printf("    [%c %s] %s\n", lvl, tag, b);
}
// Berry: every request line is recorded
static char g_bvm[256][260];
static int g_nbvm;
bool bvm_line(int k, const char *fmt, ...)
{
    char b[250];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (g_nbvm < 256) snprintf(g_bvm[g_nbvm++], 260, "k%d %s", k, b);
    return true;
}
unsigned long bvm_next_id(void) { static unsigned long id; return ++id; }
bool bvm_slot_ok(int k) { return k >= 0 && k < 11; }
const char *bvm_class_model(int k) { return k == 0 ? "TRADFRIbulbE27WWclear250lm" : k == 1 ? "TRADFRI bulb E27 WS clear 806lm" : ""; }
const char *bvm_class_seg(int k) { return k == 0 ? "0101646401016400003c" : k == 1 ? "0101646401016400003c03e82710000a8c3700000a8c" : ""; }
// Zigbee: requests are recorded and go out (ESP_OK)
static int g_reads, g_active_ep, g_nsd;
static uint8_t g_sd_req[64];
static int64_t g_setup_t[64];
static uint16_t g_setup_a[64];
static int g_nsetup;
esp_err_t zb_onoff(uint16_t d, uint8_t e, int w) { (void)d; (void)e; (void)w; return ESP_OK; }
esp_err_t zb_level(uint16_t d, uint8_t e, uint8_t l, uint16_t t) { (void)d; (void)e; (void)l; (void)t; return ESP_OK; }
static uint16_t g_read_addr = 0xFFFF;
static int g_reads_to;
esp_err_t zb_read(uint16_t d, uint8_t e, uint16_t c, const uint16_t *a, int n) { (void)e; (void)c; (void)a; (void)n; g_reads++; g_reads_to += d == g_read_addr; return ESP_OK; }
esp_err_t zb_write_u8(uint16_t d, uint8_t e, uint16_t c, uint16_t a, uint8_t t, uint8_t v) { (void)d; (void)e; (void)c; (void)a; (void)t; (void)v; return ESP_OK; }
esp_err_t zb_write_u16(uint16_t d, uint8_t e, uint16_t c, uint16_t a, uint8_t t, uint16_t v) { (void)d; (void)e; (void)c; (void)a; (void)t; (void)v; return ESP_OK; }
esp_err_t zb_setup_reports(uint16_t d, uint8_t e) { (void)e; if (g_nsetup < 64) { g_setup_t[g_nsetup] = g_now; g_setup_a[g_nsetup++] = d; } return ESP_OK; }
esp_err_t zb_write_attr(uint16_t d, uint8_t e, uint16_t c, uint16_t a, uint8_t t, uint32_t v, uint8_t s, uint16_t m) { (void)d; (void)e; (void)c; (void)a; (void)t; (void)v; (void)s; (void)m; return ESP_OK; }
esp_err_t zb_level_move(uint16_t d, uint8_t e, bool u, uint8_t r, bool w) { (void)d; (void)e; (void)u; (void)r; (void)w; return ESP_OK; }
esp_err_t zb_level_stop(uint16_t d, uint8_t e, bool w) { (void)d; (void)e; (void)w; return ESP_OK; }
esp_err_t zb_color_temp(uint16_t d, uint8_t e, uint16_t m, uint16_t t) { (void)d; (void)e; (void)m; (void)t; return ESP_OK; }
esp_err_t zb_color_xy(uint16_t d, uint8_t e, uint16_t x, uint16_t y, uint16_t t) { (void)d; (void)e; (void)x; (void)y; (void)t; return ESP_OK; }
esp_err_t zb_color_temp_move(uint16_t d, uint8_t e, int di, uint16_t r) { (void)d; (void)e; (void)di; (void)r; return ESP_OK; }
esp_err_t zb_identify(uint16_t d, uint8_t e, uint16_t s) { (void)d; (void)e; (void)s; return ESP_OK; }
esp_err_t zb_active_ep(uint16_t a) { (void)a; g_active_ep++; return ESP_OK; }
static int g_node_req;
esp_err_t zb_node_desc(uint16_t a) { (void)a; g_node_req++; return ESP_OK; }
esp_err_t zb_simple_desc(uint16_t a, uint8_t ep) { (void)a; if (g_nsd < 64) g_sd_req[g_nsd++] = ep; return ESP_OK; }
// device server
void vdev_ko_out(int inst, int ko, const uint8_t *v, int n, bool initial, bool status) { (void)inst; (void)ko; (void)v; (void)n; (void)initial; (void)status; }
void vdev_model_event(int inst) { (void)inst; }
static int g_note_inst = -1, g_note_on = -1, g_nnote;
void vdev_avail_note(int inst, bool on) { g_note_inst = inst; g_note_on = on; g_nnote++; }
typedef struct { uint16_t zaddr; int inst, k; uint8_t zep; } sdev_t;
static sdev_t g_dev[16];
static int g_ndev, g_moves;
void vdev_set_endpoint(uint16_t zaddr, uint8_t ep)   // as vdev.c: keeps it, apply() moves the class
{
    for (int i = 0; i < g_ndev; i++) {
        if (g_dev[i].zaddr != zaddr || g_dev[i].zep == ep) continue;
        g_dev[i].zep = ep;
        g_moves++;
        if (g_dev[i].k >= 0) zlink_set(g_dev[i].inst, g_dev[i].k, zaddr, ep);
    }
}
uint8_t vdev_endpoint_of(uint16_t zaddr)
{
    for (int i = 0; i < g_ndev; i++) if (g_dev[i].zaddr == zaddr) return g_dev[i].zep;
    return 0;
}
static int g_cap_calls = 0, g_cap_last = -1;
void vdev_set_capability_of(uint16_t zaddr, uint8_t capability) { (void)zaddr; g_cap_calls++; g_cap_last = capability; }

static void drain(void)
{
    g_in_task = 1;
    if (setjmp(g_jb) == 0) link_task(NULL);
    g_in_task = 0;
}
static void advance(int64_t us)   // let time pass; timers fire in order, the link task runs after each
{
    int64_t end = g_now + us;
    for (;;) {
        struct stub_timer *n = NULL;
        for (int i = 0; i < g_nt; i++) {
            if (g_t[i].armed && g_t[i].due <= end && (!n || g_t[i].due < n->due)) n = &g_t[i];
        }
        if (!n) break;
        g_now = n->due;
        if (n->period) n->due += (int64_t)n->period; else n->armed = false;
        n->cb(n->arg);
        drain();
    }
    g_now = end;
}
static int g_pass, g_fail;
static void ck(int ok, const char *what) { if (ok) g_pass++; else { g_fail++; printf("FAIL: %s\n", what); } }
static ep_desc_t mk(uint8_t ep, uint16_t profile, const uint16_t *in, int nin)
{
    ep_desc_t d;
    memset(&d, 0, sizeof d);
    d.ep = ep; d.profile = profile; d.nin = (uint8_t)nin;
    if (nin) memcpy(d.cl, in, (size_t)nin * 2);
    return d;
}
static int bvm_has(const char *s) { for (int i = 0; i < g_nbvm; i++) if (strstr(g_bvm[i], s)) return i + 1; return 0; }
static const uint16_t BASIC[] = {0x0000, 0x0003},
                      PLUG[] = {0x0000, 0x0003, 0x0004, 0x0005, 0x0006},
                      LIGHT[] = {0x0000, 0x0003, 0x0004, 0x0005, 0x0006, 0x0008, 0x0300};
static void dev(uint16_t za, int inst, int k, uint8_t ep) { g_dev[g_ndev++] = (sdev_t){za, inst, k, ep}; zlink_add(inst, k, za, ep); drain(); }
static void node_ok(uint16_t za) { zlink_disc_node(za, 0x8e, 0); drain(); }

int main(void)
{
    g_verbose = 0;
    zlink_start();

    printf("1 a light on endpoint 11 that was taken on 1: complete interview, moves to 11\n");
    dev(0x1234, 100, 0, 1);
    zlink_discover(0x1234);
    drain();
    ck(g_node_req == 1, "the interview asks for the node descriptor first");
    node_ok(0x1234);
    ck(g_cap_calls == 1 && g_cap_last == 0x8e, "its MAC capability goes to the device server");
    uint8_t e1[] = {11, 242};
    zlink_disc_eps(0x1234, e1, 2, 0);
    drain();
    ep_desc_t d = mk(11, 0x0104, LIGHT, 7);
    zlink_disc_desc(0x1234, &d, 0);
    drain();
    ck(g_dev[0].zep == 11 && g_moves == 1, "moved to 11");

    printf("2 an error for one Simple_Desc_req: a plug on endpoint 2 stays there\n");
    dev(0x2222, 101, 2, 2);
    zlink_discover(0x2222);
    drain();
    node_ok(0x2222);
    uint8_t e2[] = {1, 2};
    zlink_disc_eps(0x2222, e2, 2, 0);
    drain();
    d = mk(1, 0x0104, BASIC, 2);
    zlink_disc_desc(0x2222, &d, 0);
    drain();
    d = mk(2, 0, NULL, 0);
    zlink_disc_desc(0x2222, &d, 0x0007);
    drain();
    ck(g_dev[1].zep == 2, "incomplete interview: the plug keeps endpoint 2");

    printf("2b the answer for endpoint 2 never comes: the same\n");
    zlink_discover(0x2222);
    drain();
    node_ok(0x2222);
    zlink_disc_eps(0x2222, e2, 2, 0);
    drain();
    d = mk(1, 0x0104, BASIC, 2);
    zlink_disc_desc(0x2222, &d, 0);
    drain();
    advance(30000000);
    ck(g_dev[1].zep == 2 && !s_disc.busy, "no answer in time: keeps endpoint 2, the interview is over");

    printf("2c complete, and the plug's own endpoint is the best: stays\n");
    zlink_discover(0x2222);
    drain();
    node_ok(0x2222);
    zlink_disc_eps(0x2222, e2, 2, 0);
    drain();
    d = mk(1, 0x0104, BASIC, 2);
    zlink_disc_desc(0x2222, &d, 0);
    drain();
    d = mk(2, 0x0104, PLUG, 5);
    zlink_disc_desc(0x2222, &d, 0);
    drain();
    ck(g_dev[1].zep == 2, "keeps 2");

    printf("3 a second answer for the same endpoint does not advance the interview\n");
    dev(0x3333, 102, 2, 3);
    g_nsd = 0;
    zlink_discover(0x3333);
    drain();
    node_ok(0x3333);
    uint8_t e3[] = {1, 2, 3};
    zlink_disc_eps(0x3333, e3, 3, 0);
    drain();
    d = mk(1, 0x0104, BASIC, 2);
    zlink_disc_desc(0x3333, &d, 0);
    drain();
    zlink_disc_desc(0x3333, &d, 0);   // the same answer once more
    drain();
    d = mk(2, 0x0104, BASIC, 2);
    zlink_disc_desc(0x3333, &d, 0);
    drain();
    d = mk(3, 0x0104, LIGHT, 7);
    zlink_disc_desc(0x3333, &d, 0);
    drain();
    ck(g_nsd == 3 && g_dev[2].zep == 3, "three requests, three answers taken, the light stays on 3");

    printf("4 an off device rejoins under another short address: told as back\n");
    dev(0x4444, 103, 0, 1);
    advance(5000000);
    zlink_sent(0x4444, 0xA7);
    zlink_sent(0x4444, 0xA7);
    drain();
    ck(g_note_inst == 103 && g_note_on == 0, "OFF told to the gateway");
    int notes = g_nnote;
    zlink_set(103, 0, 0x5555, 1);
    drain();
    ck(zlink_is_off(103), "still off under the new address until it is heard there");
    g_nbvm = 0;
    zlink_heard(0x5555);
    drain();
    int a = bvm_has("availability");
    ck(g_nnote == notes + 1 && g_note_on == 1, "ON told to the gateway");
    ck(a && strstr(g_bvm[a - 1], "\"value\":true,\"initial\":false") != NULL, "the class gets true as a change");

    printf("4b a download gives the instance another class while its device is off: still off, told once\n");
    dev(0x4600, 104, 0, 1);
    advance(5000000);
    zlink_sent(0x4600, 0xA7);
    zlink_sent(0x4600, 0xA7);
    drain();
    ck(zlink_is_off(104) && g_note_inst == 104 && g_note_on == 0, "OFF told to the gateway");
    notes = g_nnote;
    int off0 = zlink_unavailable();
    zlink_pause(104);
    ck(zlink_unavailable() == off0, "while its link is down for the new class it still counts as off (the gateway's fault)");
    g_nbvm = 0;
    zlink_add(104, 1, 0x4600, 1);
    drain();
    a = bvm_has("availability");
    ck(zlink_is_off(104) && zlink_unavailable() == off0, "linked again: off, not unknown");
    ck(a && strstr(g_bvm[a - 1], "\"value\":false,\"initial\":true") != NULL, "the new class gets false as the value it starts with");
    zlink_sent(0x4600, 0xA7);
    zlink_sent(0x4600, 0xA7);
    zlink_sent(0x4600, 0xA7);
    drain();
    advance(20000000);
    ck(g_nnote == notes, "the sends that fail after linking tell nothing a second time");
    zlink_heard(0x4600);
    drain();
    ck(g_nnote == notes + 1 && g_note_on == 1 && !zlink_is_off(104), "heard again: ON told, as a return");
    zlink_sent(0x4600, 0xA7);
    zlink_sent(0x4600, 0xA7);
    drain();
    zlink_pause(104);
    zlink_add(104, 1, 0x4601, 1);   // another device takes its place
    drain();
    ck(!zlink_is_off(104), "paused while off, but another device comes in its place: nothing is known of that one");
    zlink_sent(0x4601, 0xA7);
    zlink_sent(0x4601, 0xA7);
    drain();
    off0 = zlink_unavailable();
    zlink_pause(104);
    zlink_del(104);
    ck(zlink_unavailable() == off0 - 1, "paused and then deleted: it no longer counts as off");
    dev(0x4700, 105, 0, 1);
    zlink_heard(0x4700);
    drain();
    g_nbvm = 0;
    zlink_set(105, 1, 0x4700, 1);   // it moves to the VM of another class and keeps its link
    drain();
    a = bvm_has("availability");
    ck(a && strstr(g_bvm[a - 1], "k1 ") != NULL && strstr(g_bvm[a - 1], "\"value\":true,\"initial\":true") != NULL,
       "moved to another class: the new VM gets what is known as the value to start with");

    printf("5 three lamps of one circuit come back together: their setup one after the other\n");
    uint16_t lamps[] = {0x6001, 0x6002, 0x6003};
    for (int i = 0; i < 3; i++) dev(lamps[i], 110 + i, 0, 1);
    advance(20000000);
    for (int i = 0; i < 3; i++) { zlink_sent(lamps[i], 0xA7); zlink_sent(lamps[i], 0xA7); }
    drain();
    g_nsetup = 0;
    for (int i = 0; i < 3; i++) zlink_heard(lamps[i]);
    drain();
    advance(30000000);
    ck(g_nsetup == 3 && g_setup_t[0] != g_setup_t[1] && g_setup_t[1] != g_setup_t[2], "three setups at three moments");

    printf("6 queue headroom: a colour lamp answers right after linking, nothing is lost\n");
    g_dropped[0] = 0;
    dev(0x7001, 120, 1, 1);
    uint8_t on[] = {1};
    zlink_heard(0x7001);
    zlink_attr(0x7001, 0x0006, 0x0000, 0x10, on);
    zlink_heard(0x7001);
    uint8_t u16[] = {0x5a, 0x01}, mode[] = {2};
    static const uint16_t at[] = {0x0007, 0x400B, 0x400C, 0x0003, 0x0004};
    for (int i = 0; i < 5; i++) zlink_attr(0x7001, 0x0300, at[i], 0x21, u16);
    zlink_attr(0x7001, 0x0300, 0x0008, 0x30, mode);
    zlink_attr(0x7001, 0x0300, 0x400A, 0x19, u16);
    ck(g_dropped[0] == 0, "no value dropped");
    drain();

    printf("7 a device that left the network: off at once, not asked, frames from its old address do not count\n");
    dev(0x8001, 130, 0, 1);
    zlink_heard(0x8001);
    drain();
    ck(!zlink_is_off(130), "on");
    zlink_gone(130, true);
    drain();
    ck(zlink_is_off(130) && g_note_inst == 130 && g_note_on == 0, "gone: off, told");
    g_read_addr = 0x8001;
    g_reads_to = 0;
    advance(60LL * 60 * 1000000);
    ck(g_reads_to == 0, "an hour without a question to its old address");
    zlink_heard(0x8001);   // another device got that address
    drain();
    ck(zlink_is_off(130), "a frame from its old address does not bring it back");
    zlink_gone(130, false);   // its own announce at that address (vdev_seen)
    drain();
    ck(!zlink_is_off(130) && g_note_inst == 130 && g_note_on == 1, "back: on, told");

    printf("zlink checks: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail != 0;
}
