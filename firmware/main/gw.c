// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dirk Tostmann
// The gateway's own KNX device (vdev.c, slot VDEV_GW): application 0xC000 "Zigbee gateway" under M-00FA, loaded by
// ETS like any other device. It takes over what only the console could set so far.
//
//   Parameters (segment of 5 octets; application versions 1 and 2 have the first 4):
//     0  binding from ETS: 0 off, 1 on (pair.c: the network opens when ETS asks for the programming button)
//     1  first and 2 last device number of the addresses for Zigbee devices, in the gateway's own line
//     3  pairing window in seconds, for the object and the button
//     4  LongFrames: how long the management frames of every device and the gateway may be (PID 56). 0 standard frames
//        only (15), 1 long frames of up to 55 octets (what every TP1 coupler has to pass on, 06 Profiles 3.2), 2 of up
//        to 66. Versions 1 and 2: standard frames. Applies from the restart that ends the gateway's download.
//   A download whose parameters the gateway cannot take (first above last, no device addresses, an unknown LongFrames)
//   ends in Error with a configuration fault, so ETS shows it instead of the gateway keeping the old values.
//   Objects:
//     1  pairing start/stop (1.010, write): opens the network; the first device that joins waits for ETS
//     2  pairing active (1.011, status): the network is open because this gateway opened it
//     3  Zigbee devices with an individual address (5.010, status)
//     4  Zigbee devices waiting for an individual address (5.010, status)
//   Objects of application version 2 (how full the gateway is, and why it refused something; ETS does not show
//   PID_ERROR_CODE). Version 1 has no group object table entries for them, nothing is sent then.
//     5  free places for Zigbee devices (5.010, status)
//     6  free heap, 7 its minimum since the start, 8 largest free block (12.001, octets; read only, never sent by
//        themselves: they change all the time)
//     9  VMs loaded: built-in classes and templates (5.010, status)
//    10  records' storage devsrv in use (5.001, status)
//    11  last message (16.001, 14 characters): "<address> <code>", codes RAM, CRC, CONF, SW, LOAD (a download
//        refused or not running, DPT 20.011 behind it), MODEL (Zigbee model not listed), FULL (a Zigbee device or an
//        ETS question refused: no place free), OFF (the Zigbee device does not answer), ON (it answers again), DEL
//        (removed after ETS unloaded its address), FLASH (a record does not get into the flash, or there is no
//        storage; without an address: the partition as a whole), DUP (an individual address that another of our
//        devices has already was not taken), GA (more different group addresses than the link acknowledges: this
//        device's are not all acknowledged)
//    12  fault (1.005, status): 1 while an application is in Error or does not run (the gateway's own included),
//        while a linked Zigbee device does not answer, while a record waits after a failed write or there is no
//        storage, or after a refusal for want of a place until a place is free again. A gateway that is merely
//        full is no fault: object 5 shows that.
//   Every message goes on the bus, each in turn (two in half a second are two telegrams, the same one twice is sent
//   twice). Objects 11 and 12 go out once more after the start and after every restart of the gateway's own device:
//   a fault present at the start, or one that came while the object had no group address yet, reaches the bus then.
//   The other status objects send a change; their value at the start is kept for a read.
//   Button S1: a short press switches the gateway's programming mode (LED on while it is); held for 3 s it opens the
//   pairing window (LED blinks while the network is open). A short flash every second: one of the virtual devices is
//   in programming mode (an address assignment under way, or the ETS diagnostics switched its "device LED" on). Every
//   programming mode, the gateway's as well, ends by itself after 4 minutes (vdev.c, PROG_US).
//
// Before its first download the gateway does not bind from ETS: an address assignment on the line opens nothing
// (pair.c, point 12). The pairing window (button S1, console) opens for 180 s; the range and "pair on|off" of the
// console are kept until the download sets them.
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "app.h"
#include "board.h"

static const char *TAG = "gw";

#define BTN_PIN      ((gpio_num_t)BOARD_BTN_PIN)   // S1, active low (board.h: GPIO9 on the TUL32, GPIO14 on the TULX32)
#define LED_PIN      ((gpio_num_t)BOARD_LED_PIN)   // LED, active low
#define STEP_MS      20
#define PRESS_MIN_US 40000        // shorter is a bounce
#define SHORT_MAX_US 1000000      // a press up to this long switches programming mode
#define HOLD_US      3000000      // held this long: pairing window
#define STATUS_US    500000
#define SLOW_US      5000000      // heap and storage: often enough to read, not worth more
#define WINDOW_DEF_S 180

enum {
    KO_PAIR = 1, KO_PAIR_ACTIVE, KO_BOUND, KO_WAITING,                            // version 1
    KO_FREE, KO_HEAP, KO_HEAP_MIN, KO_HEAP_BLOCK, KO_VMS, KO_STORE, KO_NOTE, KO_FAULT   // version 2
};
#define NOTE_LEN 14   // DPT 16.001

// What the last download set; written in the device server's task, read in the button task
static volatile bool s_have, s_started;
static volatile uint8_t s_binding = 1, s_first, s_last, s_window = WINDOW_DEF_S, s_frames;
static volatile uint16_t s_ia = 0xFFFF;
// Fault and the last message go on the bus once more after the start and after every restart of the gateway's own
// device (gw_params(): the end of its download). A download may just have given them their group addresses, and a value
// put while an object had none went into its shadow only. Counted, not a flag: raised in the device server's task.
static volatile uint32_t s_tell_n = 1;

static void ia_str(uint16_t a, char out[12])
{
    snprintf(out, 12, "%u.%u.%u", a >> 12, (a >> 8) & 0x0F, a & 0xFF);
}

// the parameters to the binding (pair.c); only once pair.c is up, its storage would overwrite them otherwise
static void push(const char *why)
{
    if (!s_started || !s_have) {
        return;
    }
    uint16_t line = s_ia & 0xFF00, lo = line | s_first, hi = line | s_last;
    char a[12], b[12], g[12];
    ia_str(lo, a);
    ia_str(hi, b);
    ia_str(s_ia, g);
    pair_gw_loaded();
    pair_enable(s_binding);
    int r = pair_range(lo, hi);
    if (r) {   // the download was checked (gw_params_check); a new address of the gateway can still move it out
        char t[128];
        snprintf(t, sizeof t, "addresses %s .. %s for Zigbee devices refused (%s), the range stays as it was", a, b,
                 r == 1 ? "first above last" : "not addresses of a device");
        ESP_LOGE(TAG, "%s: %s", why, t);
        gw_note(GW_NOTE_REFUSED, s_ia, KNX_ERR_CONFIG, t);
    }
    ESP_LOGI(TAG, "%s: gateway %s, binding from ETS %s, Zigbee devices get %s .. %s, pairing window %u s", why, g,
             s_binding ? "on" : "off", a, b, s_window);
    if (s_ia != 0xFFFF && (s_ia & 0xFF) >= s_first && (s_ia & 0xFF) <= s_last) {
        ESP_LOGW(TAG, "the gateway's own address %s lies in the range for Zigbee devices: give it another one", g);
    }
}

// PID 56 that a value of LongFrames (octet 4) stands for; versions 1 and 2 of the application have no such octet
static int max_apdu_of(const uint8_t *p, size_t n)
{
    return n >= 5 && p[4] == 2 ? 66 : n >= 5 && p[4] == 1 ? 55 : 15;
}

uint8_t gw_params_check(const uint8_t *p, size_t n, uint16_t ia, char *why, size_t wn)
{
    why[0] = 0;
    if (n < 4 || !p) {
        snprintf(why, wn, "parameter segment of %u B, 4 at least", (unsigned)n);
        return KNX_ERR_CONFIG;
    }
    uint16_t line = ia & 0xFF00, lo = line | p[1], hi = line | p[2];
    if (p[1] > p[2]) {
        snprintf(why, wn, "addresses for Zigbee devices: first device number %u above the last %u", p[1], p[2]);
        return KNX_ERR_CONFIG;
    }
    if (lo == 0 || hi == 0xFFFF) {
        snprintf(why, wn, "addresses for Zigbee devices: 0.0.0 and 15.15.255 are not addresses of a device");
        return KNX_ERR_CONFIG;
    }
    if (n >= 5 && p[4] > 2) {
        snprintf(why, wn, "LongFrames %u: 0, 1 or 2", p[4]);
        return KNX_ERR_CONFIG;
    }
    return 0;
}

void gw_params(const uint8_t *p, size_t n, uint16_t ia)   // the device server's task, after a download or a start
{
    if (n < 4) {
        ESP_LOGW(TAG, "parameter segment of %u B, 4 expected: not applied", (unsigned)n);
        return;
    }
    s_binding = p[0] ? 1 : 0;
    s_first = p[1];
    s_last = p[2];
    s_window = p[3] < 10 ? 10 : p[3];
    s_frames = n >= 5 ? p[4] : 0;
    // every device and the gateway, from now on: this restart ended the gateway's connection, ETS reads PID 56 anew
    // with the next one. An override at the console lasts until here.
    vdev_max_apdu(max_apdu_of(p, n));
    s_ia = ia;
    s_have = true;
    s_tell_n++;   // fault and last message once more: this download may have given them group addresses
    push("parameters");
}

void gw_ia(uint16_t ia)   // the gateway got another individual address: the range moves with its line
{
    s_ia = ia;
    push("new address");
}

void gw_knx_write(int ko, const uint8_t *v, int n)   // the device server's task: never block
{
    if (ko != KO_PAIR || n < 1) {
        return;
    }
    bool on = v[n - 1] & 1;
    ESP_LOGI(TAG, "object %d (pairing) = %d", ko, on);
    pair_window(on ? s_window : 0);
}

static void led(bool on)
{
    gpio_set_level(LED_PIN, on ? 0 : 1);
}

static void put(int ko, int value, int how)
{
    uint8_t v = (uint8_t)value;
    vdev_gw_put(ko, &v, 1, how);
}

static void put32(int ko, uint32_t x)   // DPT 12.001; only kept for a read, never sent by itself
{
    uint8_t v[4] = {(uint8_t)(x >> 24), (uint8_t)(x >> 16), (uint8_t)(x >> 8), (uint8_t)x};
    vdev_gw_put(ko, v, 4, VDEV_KEEP);
}

static void put_text(int ko, const char *t, int how)   // DPT 16.001: 14 octets, the rest 00
{
    uint8_t v[NOTE_LEN] = {0};
    memcpy(v, t, strnlen(t, NOTE_LEN));
    vdev_gw_put(ko, v, NOTE_LEN, how);
}

// ---------------------------------------------------------------- what the gateway has to tell
// Written from the device server's task and from pair.c, read by the gateway's task and the console
static portMUX_TYPE s_note_mx = portMUX_INITIALIZER_UNLOCKED;
typedef struct {
    int kind;
    uint16_t ia;
    uint8_t err;
    char why[128];
    int64_t t;
} note_t;
static note_t s_note;                  // the last one, for the console
static uint32_t s_notes[GW_NOTE_LAST + 1];
static volatile bool s_full_refused;   // something was refused for want of a place, and none has been free since
// Every message goes on the bus, in turn: the ones since the last round wait here (more than NOTE_Q in half a second
// are counted and dropped, the oldest first)
#define NOTE_Q 8
typedef struct {
    uint8_t kind, err;
    uint16_t ia;
} nq_t;
static nq_t s_nq[NOTE_Q];
static uint32_t s_nq_head, s_nq_sent, s_nq_lost;   // messages written, put on the object; dropped

// The fault object: an application in Error or not running, a linked Zigbee device that does not answer, a record that
// does not get into the flash (or no storage at all), or a refusal for want of a place while there is still none. Not
// the full gateway as such.
static int fault_now(int free_places)
{
    if (free_places > 0) {
        s_full_refused = false;
    }
    return vdev_faulty() > 0 || zlink_unavailable() > 0 || vdev_store_failing() || zb_network_lost() ||
           (s_full_refused && free_places == 0);
}

void gw_note(int kind, uint16_t ia, uint8_t err, const char *why)
{
    if (kind < GW_NOTE_REFUSED || kind > GW_NOTE_LAST) {
        return;
    }
    if (kind == GW_NOTE_FULL) {
        s_full_refused = true;
    }
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_note_mx);
    s_note.kind = kind;
    s_note.ia = ia;
    s_note.err = err;
    snprintf(s_note.why, sizeof s_note.why, "%s", why ? why : "");
    s_note.t = now;
    s_notes[kind]++;
    s_nq[s_nq_head % NOTE_Q] = (nq_t){.kind = (uint8_t)kind, .err = err, .ia = ia};
    s_nq_head++;
    portEXIT_CRITICAL(&s_note_mx);
}

static const char *err_name(uint8_t e)   // DPT 20.011, the codes this firmware gives
{
    switch (e) {
    case KNX_ERR_NONE: return "no fault";
    case KNX_ERR_CONFIG: return "configuration fault";
    case KNX_ERR_SOFTWARE: return "software fault";
    case KNX_ERR_NO_RAM: return "insufficient volatile memory";
    case KNX_ERR_ALLOC_0: return "allocation with size 0";
    case KNX_ERR_CRC: return "CRC error";
    case KNX_ERR_TOO_LONG: return "maximal table length exceeded";
    case KNX_ERR_LOAD_CMD: return "undefined load command";
    default: return "other";
    }
}

static const char *err_code(uint8_t e)   // at most 4 characters, for the message object
{
    switch (e) {
    case KNX_ERR_CONFIG: return "CONF";
    case KNX_ERR_SOFTWARE: return "SW";
    case KNX_ERR_NO_RAM: return "RAM";
    case KNX_ERR_CRC: return "CRC";
    case KNX_ERR_ALLOC_0:
    case KNX_ERR_TOO_LONG:
    case KNX_ERR_LOAD_CMD: return "LOAD";
    default: return "ERR";
    }
}

// A message as object 11 shows it, "15.15.2 RAM"; at most 14 characters: a three-digit device number with MODEL loses
// the space. Empty for none.
static void note_text(int kind, uint16_t ia, uint8_t err, char out[NOTE_LEN + 1])
{
    out[0] = 0;
    if (!kind) {
        return;
    }
    const char *code = kind == GW_NOTE_MODEL     ? "MODEL"
                       : kind == GW_NOTE_FULL    ? "FULL"
                       : kind == GW_NOTE_OFFLINE ? "OFF"
                       : kind == GW_NOTE_ONLINE  ? "ON"
                       : kind == GW_NOTE_REMOVED ? "DEL"
                       : kind == GW_NOTE_STORE   ? "FLASH"
                       : kind == GW_NOTE_DUP     ? "DUP"
                       : kind == GW_NOTE_GROUPS  ? "GA"
                       : kind == GW_NOTE_NET     ? "NET"
                                                 : err_code(err);
    if (ia == 0xFFFF) {
        snprintf(out, NOTE_LEN + 1, "%s", code);
        return;
    }
    char a[12];
    ia_str(ia, a);
    snprintf(out, NOTE_LEN + 1, strlen(a) + 1 + strlen(code) <= NOTE_LEN ? "%s %s" : "%s%s", a, code);
}

// Every message since the last round goes on the bus, in turn and in any case: two in one round are two telegrams,
// the same message twice is sent twice. tell: the last one once more, if nothing newer came (after the start or a
// restart of the gateway's device). Before the first message the object holds an empty text for a read.
static void notes_out(bool tell, bool first)
{
    nq_t q[NOTE_Q];
    int n = 0;
    portENTER_CRITICAL(&s_note_mx);
    uint32_t from = s_nq_sent;
    if (s_nq_head - from > NOTE_Q) {
        s_nq_lost += s_nq_head - from - NOTE_Q;
        from = s_nq_head - NOTE_Q;
    }
    for (uint32_t i = from; i != s_nq_head; i++) {
        q[n++] = s_nq[i % NOTE_Q];
    }
    s_nq_sent = s_nq_head;
    nq_t last = {.kind = (uint8_t)s_note.kind, .err = s_note.err, .ia = s_note.ia};
    portEXIT_CRITICAL(&s_note_mx);
    char t[NOTE_LEN + 1];
    for (int i = 0; i < n; i++) {
        note_text(q[i].kind, q[i].ia, q[i].err, t);
        put_text(KO_NOTE, t, VDEV_SEND);
    }
    if (!n && tell && last.kind) {
        note_text(last.kind, last.ia, last.err, t);
        put_text(KO_NOTE, t, VDEV_SEND);
    } else if (!n && first) {
        put_text(KO_NOTE, "", VDEV_KEEP);
    }
}

// What the objects were last given; the gateway's task only
static int s_open = -1, s_bound = -1, s_waiting = -1, s_free_n = -1, s_vms_n = -1, s_store_n = -1, s_fault_n = -1;
static int64_t s_slow_t;
static uint32_t s_told_n;   // s_tell_n as the last round saw it

static void status_round(bool first)   // every STATUS_US in the gateway's task
{
    int64_t now = esp_timer_get_time();
    int how = first ? VDEV_KEEP : VDEV_STATUS;   // the first values are only kept (answered on a read)
    uint32_t tn = s_tell_n;
    bool tell = tn != s_told_n;
    s_told_n = tn;
    int is_open = pair_is_open(), b, w;
    vdev_counts(&b, &w);
    if (is_open != s_open) {
        put(KO_PAIR_ACTIVE, is_open, how);
        s_open = is_open;
    }
    if (b != s_bound) {
        put(KO_BOUND, b, how);
        s_bound = b;
    }
    if (w != s_waiting) {
        put(KO_WAITING, w, how);
        s_waiting = w;
    }
    int fr = vdev_free_slots(), vms = bvm_vm_count(), fault = fault_now(fr);
    if (fr != s_free_n) {
        put(KO_FREE, fr > 255 ? 255 : fr, how);
        s_free_n = fr;
    }
    if (vms != s_vms_n) {
        put(KO_VMS, vms, how);
        s_vms_n = vms;
    }
    notes_out(tell, first);
    if (tell || fault != s_fault_n) {
        put(KO_FAULT, fault, tell ? VDEV_SEND : how);
        s_fault_n = fault;
    }
    if (first || now - s_slow_t >= SLOW_US) {
        s_slow_t = now;
        put32(KO_HEAP, (uint32_t)heap_caps_get_free_size(MALLOC_CAP_8BIT));
        put32(KO_HEAP_MIN, (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
        put32(KO_HEAP_BLOCK, (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        int pct = vdev_store_pct();
        if (pct >= 0 && pct != s_store_n) {
            put(KO_STORE, (pct * 255 + 50) / 100, how);   // DPT 5.001: 0..100 % as 0..255
            s_store_n = pct;
        }
    }
}

static void gw_task(void *arg)
{
    (void)arg;
    int64_t down_since = 0, status_t = 0;
    bool held = false, first = true;
    int n = 0;
    wdt_watch();
    for (;;) {
        wdt_feed();   // every 20 ms; a status round waits for the device server's lock at most
        int64_t now = esp_timer_get_time();
        bool down = gpio_get_level(BTN_PIN) == 0;
        if (down && !down_since) {
            down_since = now;
            held = false;
        } else if (down && !held && now - down_since >= HOLD_US) {
            held = true;
            ESP_LOGI(TAG, "S1 held: pairing window of %u s", s_window);
            pair_window(s_window);
        } else if (!down && down_since) {
            int64_t t = now - down_since;
            down_since = 0;
            if (!held && t >= PRESS_MIN_US && t <= SHORT_MAX_US) {
                bool on = !vdev_gw_prog();
                vdev_prog(VDEV_GW, on);
                ESP_LOGI(TAG, "S1: programming mode of the gateway %s", on ? "on" : "off");
            }
        }
        bool is_open = pair_is_open();
        // On: the gateway's own programming mode. Blinking with 0.24 s on, 0.24 s off: the Zigbee network is open.
        // A flash of 60 ms every second: one of the virtual devices is in programming mode (they have no LED).
        led(vdev_gw_prog() || (is_open && (n / 12) % 2 == 0) || (!is_open && vdev_vprog() && n % 50 < 3));
        n++;
        if (now - status_t >= STATUS_US) {
            status_t = now;
            status_round(first);
            first = false;
        }
        vTaskDelay(pdMS_TO_TICKS(STEP_MS));
    }
}

void gw_start(void)   // after pair_start(): the parameters of the last download go to the binding now
{
    gpio_config_t in = {.pin_bit_mask = 1ULL << BTN_PIN, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE};
    gpio_config_t out = {.pin_bit_mask = 1ULL << LED_PIN, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&in);
    gpio_config(&out);
    led(false);
    s_started = true;
    push("start");
    xTaskCreate(gw_task, "gw", 4096, NULL, 3, NULL);   // 3072 before version 2: it formats the message now
}

static void note_print(void)
{
    static const char *const KIND[GW_NOTE_LAST + 1] = {"", "download refused", "does not run as loaded",
                                                       "model not listed", "no virtual device free",
                                                       "Zigbee device not reachable", "Zigbee device reachable again",
                                                       "virtual device removed", "storage", "address taken",
                                                       "group addresses not acknowledged"};
    portENTER_CRITICAL(&s_note_mx);
    note_t n = s_note;
    uint32_t c[GW_NOTE_LAST + 1];
    memcpy(c, s_notes, sizeof c);
    portEXIT_CRITICAL(&s_note_mx);
    char txt[NOTE_LEN + 1];
    note_text(n.kind, n.ia, n.err, txt);
    int fr = vdev_free_slots();
    printf("gw: objects 5-12: %d places free, heap %u / minimum %u / largest block %u, %d VMs, devsrv %d %%, message "
           "'%s', fault %d (%d applications in Error or not running, %d Zigbee devices not reachable, storage %s, "
           "refused for want of a place: %s)\n", fr, (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), bvm_vm_count(), vdev_store_pct(), txt,
           fault_now(fr), vdev_faulty(), zlink_unavailable(), vdev_store_failing() ? "FAILING" : "ok",
           s_full_refused ? "yes" : "no");
    if (!n.kind) {
        printf("gw: nothing refused since the start\n");
        return;
    }
    char a[12] = "-";
    if (n.ia != 0xFFFF) {
        ia_str(n.ia, a);
    }
    printf("gw: last %lu s ago: %s, %s", (unsigned long)((esp_timer_get_time() - n.t) / 1000000), KIND[n.kind], a);
    if (n.err) {
        printf(", error %u (%s)", n.err, err_name(n.err));
    }
    printf(": %s\n", n.why);
    if (s_nq_lost) {
        printf("gw: %lu messages never reached object 11: more than %d came within half a second\n",
               (unsigned long)s_nq_lost, NOTE_Q);
    }
    printf("gw: since the start %lu downloads refused, %lu applications not running, %lu models not listed, %lu times "
           "full, %lu times a Zigbee device not reachable, %lu times back, %lu devices removed after ETS unloaded "
           "their address, %lu storage failures, %lu addresses refused as taken, %lu times more group addresses than "
           "acknowledged\n", (unsigned long)c[GW_NOTE_REFUSED],
           (unsigned long)c[GW_NOTE_FAULT], (unsigned long)c[GW_NOTE_MODEL], (unsigned long)c[GW_NOTE_FULL],
           (unsigned long)c[GW_NOTE_OFFLINE], (unsigned long)c[GW_NOTE_ONLINE], (unsigned long)c[GW_NOTE_REMOVED],
           (unsigned long)c[GW_NOTE_STORE], (unsigned long)c[GW_NOTE_DUP], (unsigned long)c[GW_NOTE_GROUPS]);
}

void gw_print(void)
{
    char g[12];
    ia_str(s_ia, g);
    if (!s_have) {
        printf("gw: no parameters loaded yet (the console settings apply), pairing window %u s, programming mode %s\n",
               s_window, vdev_gw_prog() ? "on" : "off");
        note_print();
        return;
    }
    printf("gw: %s, binding from ETS %s, Zigbee devices get device numbers %u .. %u of its line, pairing window %u s, "
           "programming mode %s, LongFrames %u (PID 56 now %d)\n",
           g, s_binding ? "on" : "off", s_first, s_last, s_window, vdev_gw_prog() ? "on" : "off", s_frames,
           vdev_max_apdu(0));
    note_print();
}
