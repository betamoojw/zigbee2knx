// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dirk Tostmann
// KNX device server on the TP bus. Every virtual device is a System B device (mask 07B0) with its own individual
// address; this one transceiver answers for all of them (knx_link.c acknowledges from the lists published here).
// ETS assigns the address in programming mode, downloads tables and parameters with the load procedure of MV-07B0
// and runs group communication, as with a real device.
//
// Port of the management part of an earlier Python implementation of this device server, which passed ETS6 over
// KNXnet/IP on 2026-11-10. It covers the services and properties ETS used there, plus two findings with ETS 6.4.1
// of 2026-11-12: a relative allocation with fill flag 01 must really fill (ETS skips runs of zeros afterwards), and
// a segment keeps its content over Unload and a new allocation without fill (partial download writes changed
// octets only and compares size and CRC in PID 27).
//
// What a virtual device does comes from its class template in Berry (bvm.c) and the Zigbee device linked to it
// (zlink.c): a group write goes into the class as knx_write, ko_status/ko_send of the class go out as group
// telegrams, the parameter segment goes into the class as params after a download and after a start.
// The template comes with the download: a product carries it signed behind its parameters (kgt.c). Once checksum
// and signature are verified it runs in a VM of its own. A segment without a template falls back to the class built
// into the firmware for that application number (C001, C002; FE20, FE21 before). A refused template changes nothing.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "psa/crypto.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "app.h"
#include "board.h"

static const char *TAG = "vdev";

#define VDEV_MAX     KNX_VDEV_MAX // virtual devices for Zigbee devices, slots 0 .. VDEV_MAX - 1
#define GW           VDEV_GW      // then the gateway's own device (gw.c): always there, no Zigbee device behind it
#define NDEV         (VDEV_MAX + 1)
// Application numbers under M-00FA: C000 the gateway itself, C001 zigbee-dimmable-light,
// C002 zigbee-white-spectrum-light, C003 zigbee-switch, C004 zigbee-remote. FE20/FE21, earlier test numbers, are
// still taken as C001/C002.
#define GW_APP       0xC000
#define KO_MAX       16
#define MASK         0x07B0
#define MANUFACTURER 0x00FA       // M-00FA (KNX Association), as open-source KNX projects use it; not registered
#define IA_UNPROG    0xFFFF       // 15.15.255
#define ZADDR_NONE   0xFFFF
#define SEG_BASE     0x4000u      // table reference of load object n: SEG_BASE + (n - 1) * SEG_SIZE
#define SEG_SIZE     0x0400u
#define BIG4_BASE    0x8000u      // a parameter segment above 1 KiB (embedded template)
#define BIG4_SIZE    0x4000u
#define MAX_APDU_DEF 15           // PID 56 by default: memory services carry up to 12 data octets, standard frames only
#define PROP_MAX     (KNX_TPDU_MAX - 6)   // data octets of a property value answer: PID 56 - 5 at most
#define ACK_US       3000000      // T_ACK timeout
// Programming mode ends by itself after this, however it was switched on, and setting it again does not start the time
// anew (03_05_01 Resources 4.26.1, "Autonomous inactivation of Programming Mode": optional, 4 minutes). A virtual
// device has no LED of its own: left on by the ETS diagnostics it would answer the next address assignment on the
// line and take the address meant for another device.
#define PROG_US      240000000LL
#define IDLE_US      6000000      // connection timeout
#define SAVE_QUIET_US 1500000     // flash is written while our own traffic is quiet for this long ...
#define SAVE_LATE_US  30000000    // ... but a change waits no longer than this once its device has no connection
#define SAVE_HARD_US  180000000   // ... and never longer than this
#define SAVE_RETRY_US 10000000    // after a write that failed; twice as long after each further one, up to 64 times
#define APPLY_RETRY_US 30000000   // a template that could not be checked or loaded for want of memory, again after this
#define APPLY_OOM_TRIES 3         // a load that ran out of memory itself: this many times, then a download or a start
#define SEG_BUDGET    (96u * 1024u)   // RAM all segments together may take
#define SEG_KEEP_US   60000000    // an unloaded object keeps its content this long for the allocation that follows
#define LOST_MAX     3            // a status value the link gave up goes out again at most this often
// After the start and after the restart of a device the bus does not know what its status objects hold: the first value
// of each goes on the bus once (tell_held). The values of one device are collected for HELD_SETTLE_US (its first reads
// answer within a second), then they go out together, one device every HELD_GAP_US, as zlink.c tells availability.
#define HELD_SETTLE_US 2000000
#define HELD_GAP_US    300000
#define PARAMS_MAX   88           // parameter octets that fit into one request line to Berry
#define NVS_PART     BOARD_DEVSRV_PART   // "devsrv" on the TUL32, "actor_db" on the TULX32 (board.h)
#define REC_MAGIC    0x33564456u  // "VDV3": the template behind the parameters may be kept apart (s_ts)
#define REC_MAGIC_2  0x32564456u  // "VDV2": with the IEEE address of the Zigbee device, still read
#define REC_MAGIC_1  0x31564456u  // "VDV1", still read
#define TS_MIN       256          // a template part smaller than this is not worth keeping apart

enum { LS_UNLOADED, LS_LOADED, LS_LOADING, LS_ERROR };
enum { LE_START = 1, LE_COMPLETE, LE_ADDITIONAL, LE_UNLOAD };

typedef struct {
    uint8_t state, run_state, pv[5];
    uint8_t err;                   // PID_ERROR_CODE (28): why the state is Error (KNX_ERR_*), else 0
    uint32_t ref, size, cap;       // cap: octets behind seg; the content survives Unload
    uint8_t *seg;
    int64_t keep_until;            // an unloaded object gives its buffer back then
    uint8_t ts1;                   // object 4 only: 0, or 1 + the entry of s_ts that holds the octets from `at` on
    uint32_t at;                   // with ts1: seg holds the octets before this (cap == at), s_ts the rest
} lobj_t;

typedef struct {
    bool on;
    uint16_t remote;
    uint8_t rx_seq, tx_seq, retries;
    int64_t last, sent;            // sent: when the head of the queue went out, 0 = nothing waits for T_ACK
    uint8_t q[4][KNX_TPDU_MAX], qn[4], qh, qc;
} conn_t;

typedef struct {
    bool used, prog, dirty;
    int64_t prog_until;                // programming mode ends by itself then (PROG_US after it was switched on)
    int64_t dirty_since, save_after;   // when the unsaved change began; no write before save_after (after an error)
    uint8_t save_fails;                // writes of its record that failed one after the other (the storage's fault)
    uint16_t ia;
    uint8_t device_control, serial[6];
    lobj_t lo[6];                  // 1..5: address table, association table, group object table, application, PEI
    conn_t c;
    int inst;                      // Berry instance, 100 + slot
    int8_t k;                      // Berry slot of the linked instance, -1 = not linked
    int8_t k_hint;                 // built-in class given by hand (console), used until a download says what it is
    bool have_ieee, reparam;       // reparam: the class got its parameters before it knew the model
    bool cap_known, rx_on;         // the MAC capability of its device announce: receiver on when idle (not sleepy)
    uint8_t reread_n;              // model questions without an answer so far: the next waits longer
    bool gone;                     // its Zigbee device left the network: the next ETS question for its address
                                   // takes a new one in its place (pair.c)
    uint8_t ieee[8];               // the Zigbee device, whatever short address it has
    int64_t reread_at;             // when to ask the device for its model again
    uint8_t tpl;                   // 0: built-in class or none, 1: embedded template runs, 2: one was refused
    uint8_t nparams;               // parameter octets the class takes
    uint8_t model_ok;              // 0: not compared, 1: the template lists the device's model, 2: it does not
    uint8_t fault;                 // the application is loaded but does not run as loaded (KNX_ERR_*): its load state
                                   // reads Error until the next download or start (RAM only)
    bool next_tpl;                 // the check at LoadCompleted found a template, next_digest is its content
    char next_digest[17];
    uint16_t next_params;          // ... and the parameter octets it takes
    bool app_leaving;              // the application ETS had loaded was unloaded or is being replaced (app_gone())
    int64_t retry_at;              // apply() again then: its template could not be checked for want of memory
    uint8_t oom_tries;             // loads that ran out of memory themselves since the last download or start
    uint16_t zaddr;                // Zigbee short address, ZADDR_NONE = none
    uint8_t zep;
    struct {
        uint8_t have : 1, unsent : 1;   // unsent: the link had no room for this value yet
        uint8_t status : 1;             // the class gave it as a status: the same value may go out twice
        uint8_t lost : 2;               // times the link gave this value up and it went out again (LOST_MAX)
        uint8_t told : 1;               // a status the bus heard since the start or the device's restart
        uint8_t due : 1;                // ... not yet: it goes out with the device's other held values (tell_held)
        uint8_t len, v[KNX_KO_VAL];
    } ko[KO_MAX + 1];
    uint8_t resp_ko;               // an answer to a read the link had no room for: its object, 0 none ...
    uint16_t resp_ga;              // ... and the group address the read came on
    int64_t held_at;               // its held status values go on the bus then (tell_held), 0: none due
    uint32_t rx, tx, grp_rx, grp_tx, unhandled, mem_wr, mem_wr_octets, mem_rejected;
} vdev_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t ia, zaddr;
    uint8_t device_control, zep;
    int8_t k;
    uint8_t flags;                 // bit 0: ieee is valid, bit 1: the Zigbee device left the network, bit 2: the
                                   // capability of its announce is known, bit 3: its receiver is on when idle
                                   // (bits 2 and 3; clear in records of earlier development versions)
    uint8_t ieee[8];               // VDV1 had no such field
    struct __attribute__((packed)) {
        uint8_t state, run_state, pv[5], err;   // err: PID 28 of an object in Error (0 in records of older firmware)
        uint32_t size;
    } o[5];
    // VDV3: object 4 is stored up to `at`; its octets from there (tlen of them, CRC-32 tcrc) are in blob ts_key()
    uint32_t at, tlen, tcrc;       // tlen 0: the whole segment is in the record
} rec_t;
#define REC2_SIZE (sizeof(rec_t) - 12)
#define REC1_SIZE (REC2_SIZE - 8)

// Two queues into the device server's task: frames from the link, which the link may have acknowledged already, and
// values from the classes, which come in bursts (a central command to all lamps). A burst of values does not push out
// frames, and the other way round.
#define FRAME_Q 24   // frames: more than TP1 carries while the task is busy with one group telegram to 32 devices
#define VALUE_Q 64   // class values: 32 lamps answering a central command with two status values each
enum { EV_FRAME, EV_LOST };   // EV_LOST: a frame of ours the link gave up (knx_link_lost)
typedef struct {
    uint8_t type;
    knx_frame_t f;
} ev_t;
typedef struct {
    int16_t inst;
    uint8_t ko, len, v[KNX_KO_VAL];
    bool initial, status;
} kv_t;

static vdev_t s_d[NDEV];
static SemaphoreHandle_t s_mx;
static QueueHandle_t s_q, s_kq;    // frames (ev_t), class values (kv_t)
static TaskHandle_t s_task;
static volatile uint8_t s_q_peak, s_kq_peak;   // the most entries that waited at once (console vdev)
static nvs_handle_t s_nvs;
static bool s_nvs_ok;
static int64_t s_last_rx;          // last frame that was meant for one of our devices
static bool s_v8_blocked;          // the gateway's old record "v8" is not moved yet (gw_key_move())
static bool s_hit;                 // set while a frame is dispatched: one of our devices took it
static volatile uint8_t s_test_refuse;   // console tplfail: the next check at LoadCompleted fails with this
// PID 56 (max APDU length) of every device: ETS writes memory in pieces of this minus 3 octets. Above 15 the pieces
// come in extended frames, up to 63 data octets (KNX_TPDU_MAX - 1 = 66). The gateway's parameter LongFrames sets it at
// every start and every restart of the gateway's device (gw.c); the console `apdu` overrides it until then.
static uint8_t s_max_apdu = MAX_APDU_DEF;
static uint32_t s_ev_lost, s_kv_lost, s_saves, s_save_errs;   // frames / class values dropped: queue full
static int64_t s_held_t;           // when the held status values of a device went out last (one device per HELD_GAP_US)
static int s_held_rr;              // the device to look at first next time

// class 0 zigbee-dimmable-light (application C001), class 1 zigbee-white-spectrum-light (C002): order info (PID 15),
// octets of application parameters at the start of the parameter segment
static const struct {
    char order[11];
    uint8_t params;
} CLS[2] = {{"ZB-LIGHT-D", 10}, {"ZB-LIGHT-W", 22}};

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(p[0] << 8 | p[1]);
}

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint16_t crc16_step(uint16_t c, const uint8_t *p, size_t n)
{
    while (n--) {
        c ^= (uint16_t)(*p++ << 8);
        for (int i = 0; i < 8; i++) {
            c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1);
        }
    }
    return c;
}

static uint16_t crc16_ccitt(const uint8_t *p, size_t n)
{
    return crc16_step(0xFFFF, p, n);
}

static uint32_t crc32_of(const uint8_t *p, size_t n)   // CRC-32 (IEEE 802.3), the key of a kept template part
{
    uint32_t c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *p++;
        for (int i = 0; i < 8; i++) {
            c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : c >> 1;
        }
    }
    return ~c;
}

static void ia_str(uint16_t a, char out[12])
{
    snprintf(out, 12, "%u.%u.%u", a >> 12, (a >> 8) & 0x0F, a & 0xFF);
}

static void ga_str(uint16_t g, char out[12])
{
    snprintf(out, 12, "%u/%u/%u", g >> 11, (g >> 8) & 0x07, g & 0xFF);
}

static void hex(const uint8_t *p, int n, char *out)
{
    for (int i = 0; i < n; i++) {
        sprintf(out + 2 * i, "%02x", p[i]);
    }
    out[2 * n] = 0;
}

static int slot(const vdev_t *d)
{
    return (int)(d - s_d);
}

static bool is_gw(const vdev_t *d)
{
    return d == &s_d[GW];
}

static void set_prog(vdev_t *d, bool on)   // under the lock; the caller publishes
{
    if (on && !d->prog) {
        d->prog_until = esp_timer_get_time() + PROG_US;
    } else if (!on) {
        d->prog_until = 0;
    }
    d->prog = on;
}

static void mark(vdev_t *d)   // something that has to survive a restart changed
{
    if (!d->dirty) {
        d->dirty = true;
        d->dirty_since = esp_timer_get_time();
    }
}

// ---------------------------------------------------------------- templates kept once per content
// A product carries its template behind the parameters (kgt.c: base64 text, then the trailer "KGTE"). Earlier
// firmware kept a copy per device in RAM and in its record, 1.7-5.4 KB each, and the TULX32 as shipped has
// about 180 KiB for all data. Now the part of the parameter segment from the text on is kept once per
// content in s_ts, in RAM and in NVS (blob ts_key()), and every device with that product refers to it. Reading the
// segment, its CRC and the template check see the whole of it; an allocation for a new download gives the object
// its own copy again first (seg_own), so a partial download finds the content it relies on.
typedef struct {
    uint8_t *data;
    uint32_t len, crc;
    uint16_t refs;
    bool saved;                    // the blob ts_key() holds it
    // The template these octets carry passed the checks once (kgt.c): its content digest, "" not yet, and the parameter
    // octets it takes. Nobody writes into a kept part (seg_own() copies it first), so the result holds for its life.
    char digest[17];
    uint16_t params;
} tshare_t;

static tshare_t s_ts[NDEV];
static struct {
    uint32_t len, crc;
} s_tref[NDEV];                    // the template part the stored record of each slot names (len 0: none); from the
                                   // start on, restored or not, then kept up with every record written or erased
static bool s_swept;               // every record was read at the start: a part no record names may go from now on
static volatile bool s_loaded;     // the records are read (or there is no storage to read them from): zb.c may decide

// The record of slot i: "v<i>", the gateway's "vgw". Earlier firmware gave the gateway "v8", after the 8 slots it
// had; with more slots that is a virtual device's key (gw_key_move()).
static void rec_key(int i, char key[12])
{
    if (i == GW) {
        snprintf(key, 12, "vgw");
    } else {
        snprintf(key, 12, "v%d", i);
    }
}

static void ts_key(uint32_t crc, uint32_t len, char key[16])
{
    snprintf(key, 16, "t%08lx%04lx", (unsigned long)crc, (unsigned long)(len & 0xFFFF));
}

// a reference to the kept copy of these octets, made if there is none yet; -1: no room
static int ts_take(const uint8_t *p, uint32_t n, uint32_t crc, bool saved)
{
    int i, free_i = -1;
    for (i = 0; i < NDEV; i++) {
        tshare_t *t = &s_ts[i];
        if (t->refs && t->len == n && t->crc == crc && memcmp(t->data, p, n) == 0) {
            t->refs++;
            t->saved = t->saved || saved;
            return i;
        }
        if (!t->refs && free_i < 0) {
            free_i = i;
        }
    }
    if (free_i < 0) {
        return -1;
    }
    uint8_t *b = malloc(n);
    if (!b) {
        return -1;
    }
    memcpy(b, p, n);
    s_ts[free_i] = (tshare_t){.data = b, .len = n, .crc = crc, .refs = 1, .saved = saved};
    return free_i;
}

static void ts_drop(int i)   // RAM only: the blob goes when no stored record names it any more (tref_set())
{
    if (i >= 0 && i < NDEV && s_ts[i].refs && --s_ts[i].refs == 0) {
        free(s_ts[i].data);
        memset(&s_ts[i], 0, sizeof s_ts[i]);
    }
}

// Where the template text of a parameter segment begins (the trailer's text offset, kgt.c), 0: nothing to keep apart
static uint32_t tpl_split(const uint8_t *seg, uint32_t n)
{
    if (!seg || n < 16 || memcmp(seg + n - 16, "KGTE", 4) != 0) {
        return 0;
    }
    uint32_t v[3] = {0, 0, 0};
    for (int i = 0; i < 12; i++) {
        int c = seg[n - 12 + i];
        int x = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (x < 0) {
            return 0;
        }
        v[i / 4] = v[i / 4] << 4 | (uint32_t)x;
    }
    if (!(v[2] <= v[0] && v[0] + v[1] <= n - 16) || n - v[0] < TS_MIN) {
        return 0;
    }
    return v[0];
}

__attribute__((unused))   // the host tests check the objects against it
static uint32_t seg_held(const lobj_t *o)   // octets of content the object holds, its own and kept apart
{
    return o->ts1 ? o->at + s_ts[o->ts1 - 1].len : o->cap;
}

static void seg_read(const lobj_t *o, uint32_t off, uint8_t *dst, uint32_t n)   // within seg_held(o)
{
    if (o->ts1 && off + n > o->at) {
        uint32_t m = off < o->at ? o->at - off : 0;
        if (m) {
            memcpy(dst, o->seg + off, m);
        }
        memcpy(dst + m, s_ts[o->ts1 - 1].data + (off + m - o->at), n - m);
    } else if (n) {
        memcpy(dst, o->seg + off, n);
    }
}

static uint16_t seg_crc(const lobj_t *o)   // CRC-16-CCITT over the segment as ETS loaded it (PID 27)
{
    if (!o->ts1 || o->size <= o->at) {
        return crc16_ccitt(o->seg, o->size);
    }
    return crc16_step(crc16_step(0xFFFF, o->seg, o->at), s_ts[o->ts1 - 1].data, o->size - o->at);
}

// The object gets its own copy of the whole content again (before an allocation or a write into the kept part)
static bool seg_own(lobj_t *o)
{
    if (!o->ts1) {
        return true;
    }
    tshare_t *t = &s_ts[o->ts1 - 1];
    uint8_t *p = realloc(o->seg, o->at + t->len);
    if (!p) {
        return false;
    }
    memcpy(p + o->at, t->data, t->len);
    o->seg = p;
    o->cap = o->at + t->len;
    ts_drop(o->ts1 - 1);
    o->ts1 = 0;
    o->at = 0;
    return true;
}

static void seg_drop(lobj_t *o)   // the object's content goes, its own and its reference
{
    free(o->seg);
    o->seg = NULL;
    o->cap = 0;
    if (o->ts1) {
        ts_drop(o->ts1 - 1);
        o->ts1 = 0;
        o->at = 0;
    }
}

// A loaded parameter segment with a template: its template part goes to s_ts (shared with every device that has the
// same), the object keeps the octets before it. Without room it simply stays as it is.
static void seg_share(vdev_t *d)
{
    lobj_t *o = &d->lo[4];
    if (o->ts1 || o->state != LS_LOADED || d == &s_d[GW]) {
        return;
    }
    uint32_t at = tpl_split(o->seg, o->size);
    if (!at) {
        return;
    }
    uint32_t n = o->size - at;
    int i = ts_take(o->seg + at, n, crc32_of(o->seg + at, n), false);
    if (i < 0) {
        return;
    }
    uint8_t *p = realloc(o->seg, at);
    if (!p) {
        ts_drop(i);
        return;
    }
    o->seg = p;
    o->cap = at;
    o->at = at;
    o->ts1 = (uint8_t)(i + 1);
    ESP_LOGI(TAG, "dev %d: template part of %u B kept once (%u device(s) share it), %u B of its own", (int)(d - s_d),
             (unsigned)n, (unsigned)s_ts[i].refs, (unsigned)at);
}

// The template in object 4's kept part passed the checks (at LoadCompleted or when it was brought up): remembered at
// the part, so that the next restart need not check the same octets again (23 to 43 KB of heap each time)
static void tpl_checked(vdev_t *d, const char *digest, uint16_t params)
{
    lobj_t *o = &d->lo[4];
    if (o->ts1 && digest && digest[0]) {
        tshare_t *t = &s_ts[o->ts1 - 1];
        snprintf(t->digest, sizeof t->digest, "%s", digest);
        t->params = params;
    }
}

// ---------------------------------------------------------------- tables (valid only while the object is Loaded)
static int words(vdev_t *d, int obj, const uint8_t **p)
{
    lobj_t *o = &d->lo[obj];
    if (o->state != LS_LOADED || o->size < 2 || !o->seg) {
        return 0;
    }
    *p = o->seg;
    return (int)(o->size / 2);
}

static int ga_count(vdev_t *d, const uint8_t **p)   // TSAP t (1..count) -> be16(*p + 2 * t)
{
    int n = words(d, 1, p);
    if (!n) {
        return 0;
    }
    int c = be16(*p);
    return c > n - 1 ? n - 1 : c;
}

static int assoc_count(vdev_t *d, const uint8_t **p)   // entry i (0..count-1): TSAP be16(*p+2+4i), ASAP be16(*p+4+4i)
{
    int n = words(d, 2, p);
    if (!n) {
        return 0;
    }
    int c = be16(*p);
    return c > (n - 1) / 2 ? (n - 1) / 2 : c;
}

static uint16_t descriptor(vdev_t *d, int asap)
{
    const uint8_t *p;
    int n = words(d, 3, &p);
    if (!n || asap < 1 || asap > be16(p) || asap >= n) {
        return 0;
    }
    return be16(p + 2 * asap);
}

static int ga_for_asap(vdev_t *d, int asap)
{
    const uint8_t *g, *a;
    int ng = ga_count(d, &g), na = assoc_count(d, &a);
    for (int i = 0; i < na; i++) {
        int ts = be16(a + 2 + 4 * i), as = be16(a + 4 + 4 * i);
        if (as == asap && ts >= 1 && ts <= ng) {
            return be16(g + 2 * ts);
        }
    }
    return -1;
}

// Octets after the APCI octet that a group object value takes, from the value field type in its descriptor
// (03_05_01 4.18.6.2.4.1.4). 0: six bits or less, the value travels in the APCI octet.
static int go_octets(int type)
{
    static const uint8_t tab[] = {2, 3, 4, 6, 8, 10, 14, 5, 7, 9, 11, 12, 13};   // types 8..20
    if (type <= 5) {
        return 0;
    }
    if (type <= 7) {
        return 1;
    }
    return type <= 20 ? tab[type - 8] : 255;
}

static volatile bool s_vprog;   // a virtual device is in programming mode (publish() keeps it)
static int s_ga_over;           // group addresses the link did not take at the last publish()

// ---------------------------------------------------------------- acknowledge lists for the link
static void publish(void)
{
    static uint16_t ia[KNX_ACK_IA_MAX], ga[KNX_ACK_GA_MAX];
    int nia = 0, nga = 0, over = 0;
    uint16_t over_ia = 0xFFFF;   // the first device whose group addresses did not all fit
    bool any = false, vprog = false;
    for (int i = 0; i < NDEV; i++) {
        vdev_t *d = &s_d[i];
        if (!d->used) {
            continue;
        }
        any = true;
        vprog = vprog || (d->prog && !is_gw(d));
        if ((d->ia != IA_UNPROG || d->prog) && nia < KNX_ACK_IA_MAX) {   // the factory address only in programming mode
            ia[nia++] = d->ia;
        }
        const uint8_t *g;
        int ng = ga_count(d, &g);
        for (int t = 1; t <= ng; t++) {
            uint16_t x = be16(g + 2 * t);
            bool seen = false;
            for (int j = 0; j < nga && !seen; j++) {
                seen = ga[j] == x;
            }
            if (!seen && x) {
                if (nga < KNX_ACK_GA_MAX) {
                    ga[nga++] = x;
                } else {
                    over_ia = over ? over_ia : d->ia;
                    over++;
                }
            }
        }
    }
    // Frames to the others still reach the devices, but nothing here acknowledges them: a sender without another
    // listener on the line repeats each three times and counts it as failed. Told on the gateway's message object
    // when it grows (the download that brought the address too many), not at every publish.
    if (over > s_ga_over) {
        char why[96];
        snprintf(why, sizeof why, "%d group addresses more than the %d the link acknowledges", over, KNX_ACK_GA_MAX);
        ESP_LOGW(TAG, "%s", why);
        gw_note(GW_NOTE_GROUPS, over_ia, 0, why);
    }
    s_ga_over = over;
    s_vprog = vprog;   // for the gateway's LED
    knx_link_ack_lists(ia, nia, ga, nga, any);
}

// ---------------------------------------------------------------- persistence
// NVS entries a blob of n octets may take: 32 octets each, a header for every chunk (a chunk ends with its page of 126
// entries, the first starts anywhere) and the blob's index, plus the tail of a page NVS closes when the first chunk does
// not fit there (nvs_storage.cpp, writeMultiPageBlob: less than the blob, at most a tenth of a page).
static size_t nvs_need(size_t n)
{
    size_t chunks = n / 4000 + 2;
    return (n + 31) / 32 + 2 * chunks + 1 + ((n < 400 ? n : 400) + 31) / 32;
}

// Is there room for a blob of n octets? NVS does not ask before it writes: it writes what fits of a blob, marks that as
// erased again when the rest does not, and gets the space back only by erasing a page. A new blob is written whole
// before the old one of its key goes. Not known (no statistics): NVS itself decides.
static bool room(size_t n)
{
    nvs_stats_t st;
    return nvs_get_stats(NVS_PART, &st) != ESP_OK || nvs_need(n) <= st.available_entries;
}

// No stored record names this template part any more: its blob goes now, not at the next start. A copy in RAM is
// written again before the next record that names it. Before every record was read at the start, ts_sweep() decides.
static void ts_unnamed(uint32_t crc, uint32_t len)
{
    if (!len || !s_swept || !s_nvs_ok) {
        return;
    }
    for (int i = 0; i < NDEV; i++) {
        if (s_tref[i].len == len && s_tref[i].crc == crc) {
            return;
        }
    }
    char k[16];
    ts_key(crc, len, k);
    if (nvs_erase_key(s_nvs, k) == ESP_OK) {
        nvs_commit(s_nvs);
        ESP_LOGI(TAG, "template part %s: no record names it any more, deleted", k);
    }
    for (int i = 0; i < NDEV; i++) {
        if (s_ts[i].refs && s_ts[i].crc == crc && s_ts[i].len == len) {
            s_ts[i].saved = false;
        }
    }
}

static void tref_set(int i, uint32_t crc, uint32_t len)   // the stored record of slot i names this part now (len 0: none)
{
    uint32_t oc = s_tref[i].crc, ol = s_tref[i].len;
    s_tref[i].crc = crc;
    s_tref[i].len = len;
    if (ol && (ol != len || oc != crc)) {
        ts_unnamed(oc, ol);
    }
}

// A record (or the template part it needs) did not get into the flash: the change stays in RAM and is tried again,
// each time later (10 s, then twice as long up to 640 s: a full partition stays full, and every failed write of a blob
// costs erase cycles). The first failure of a series goes to the gateway's message object; its fault stays on while
// any record waits (vdev_store_failing()).
static void store_failed(vdev_t *d, int64_t now, const char *why)
{
    s_save_errs++;
    if (d->save_fails < 255) {
        d->save_fails++;
    }
    int64_t wait = (int64_t)SAVE_RETRY_US << (d->save_fails - 1 < 6 ? d->save_fails - 1 : 6);
    d->save_after = now + wait;
    ESP_LOGE(TAG, "dev %d: %s; tried again in %d s", slot(d), why, (int)(wait / 1000000));
    if (d->save_fails == 1) {
        gw_note(GW_NOTE_STORE, d->ia, 0, why);
    }
}

static void save(vdev_t *d)
{
    int64_t now = esp_timer_get_time();
    char why[96];
    if (!s_nvs_ok) {   // nothing to keep it in: vdev_store_failing() makes that the gateway's fault
        d->dirty = false;
        return;
    }
    // A kept template part is written once, before the first record that names it (a restart in between leaves a
    // blob nobody names, which the next start deletes)
    lobj_t *o4 = &d->lo[4];
    bool apart = o4->state == LS_LOADED && o4->ts1 && o4->size > o4->at;
    tshare_t *ts = apart ? &s_ts[o4->ts1 - 1] : NULL;
    esp_err_t e = ESP_OK;
    if (ts && !ts->saved) {
        char tk[16];
        ts_key(ts->crc, ts->len, tk);
        e = room(ts->len) ? nvs_set_blob(s_nvs, tk, ts->data, ts->len) : ESP_ERR_NVS_NOT_ENOUGH_SPACE;
        if (e == ESP_OK) {
            e = nvs_commit(s_nvs);
        }
        if (e != ESP_OK) {
            snprintf(why, sizeof why, "its template part (%u B) does not get into the flash: %s", (unsigned)ts->len,
                     e == ESP_ERR_NVS_NOT_ENOUGH_SPACE ? "partition full" : esp_err_to_name(e));
            store_failed(d, now, why);
            return;
        }
        ts->saved = true;
    }
    size_t n = sizeof(rec_t);
    for (int o = 1; o <= 5; o++) {
        if (d->lo[o].state == LS_LOADED) {
            n += o == 4 && apart ? o4->at : d->lo[o].size;
        }
    }
    uint8_t *b = malloc(n);
    if (!b) {
        ESP_LOGE(TAG, "dev %d: no memory to save %u B, trying again later", slot(d), (unsigned)n);
        d->save_after = now + SAVE_RETRY_US;
        return;
    }
    rec_t r = {.magic = REC_MAGIC, .ia = d->ia, .zaddr = d->zaddr, .device_control = d->device_control, .zep = d->zep,
               .k = d->k_hint, .flags = (uint8_t)((d->have_ieee ? 1 : 0) | (d->gone ? 2 : 0) |
                                                   (d->cap_known ? 4 : 0) | (d->rx_on ? 8 : 0))};
    memcpy(r.ieee, d->ieee, 8);
    if (apart) {
        r.at = o4->at;
        r.tlen = s_ts[o4->ts1 - 1].len;
        r.tcrc = s_ts[o4->ts1 - 1].crc;
    }
    uint8_t *p = b + sizeof r;
    for (int o = 1; o <= 5; o++) {
        lobj_t *l = &d->lo[o];
        bool loaded = l->state == LS_LOADED;
        // Loading is transient, and only a loaded object is stored with its content: a restart in the middle of a
        // download brings the device back unloaded and without the half-written segment.
        r.o[o - 1].state = l->state == LS_LOADING ? LS_UNLOADED : l->state;
        r.o[o - 1].run_state = l->run_state;
        r.o[o - 1].err = l->state == LS_ERROR ? l->err : 0;
        memcpy(r.o[o - 1].pv, l->pv, 5);
        r.o[o - 1].size = loaded ? l->size : 0;
        uint32_t own = o == 4 && apart ? l->at : l->size;
        if (loaded && own) {
            memcpy(p, l->seg, own);
            p += own;
        }
    }
    memcpy(b, &r, sizeof r);
    char key[12];
    rec_key(slot(d), key);
    e = room(n) ? nvs_set_blob(s_nvs, key, b, n) : ESP_ERR_NVS_NOT_ENOUGH_SPACE;
    if (e == ESP_OK) {
        e = nvs_commit(s_nvs);
    }
    free(b);
    s_saves++;
    if (e != ESP_OK) {   // stays unsaved and is tried again
        snprintf(why, sizeof why, "its record (%u B) does not get into the flash: %s", (unsigned)n,
                 e == ESP_ERR_NVS_NOT_ENOUGH_SPACE ? "partition full" : esp_err_to_name(e));
        store_failed(d, now, why);
    } else {
        if (d->save_fails) {
            ESP_LOGI(TAG, "dev %d: saved after %u failed writes", slot(d), d->save_fails);
        }
        d->save_fails = 0;
        d->dirty = false;
        ESP_LOGI(TAG, "dev %d: saved, %u B", slot(d), (unsigned)n);
        // the part the record named before goes if no other record names it: nothing else frees its flash
        tref_set(slot(d), ts ? ts->crc : 0, ts ? ts->len : 0);
    }
}

// The KNX serial number (PID 11): 00FA and the first four octets of SHA-256 over the IEEE address of the Zigbee device
// behind the virtual device, the 8 octets as its record keeps them. It follows the Zigbee device, not the slot: the
// next device in a slot that was freed got the same number before, and ETS addresses by serial number. The gateway:
// over its base MAC. A virtual device without an IEEE address (set up by hand) keeps the earlier scheme: the last three
// octets of the base MAC and the slot. Derived whenever the address changes, never stored.
static void serial_of(vdev_t *d)
{
    static bool psa_up;
    int i = slot(d);
    uint8_t mac[6] = {0}, h[32];
    size_t got = 0;
    esp_read_mac(mac, ESP_MAC_BASE);
    d->serial[0] = MANUFACTURER >> 8;
    d->serial[1] = MANUFACTURER & 0xFF;
    const uint8_t *in = i == GW ? mac : d->have_ieee ? d->ieee : NULL;
    if (in && !psa_up) {
        psa_up = psa_crypto_init() == PSA_SUCCESS;
    }
    if (in && psa_up && psa_hash_compute(PSA_ALG_SHA_256, in, i == GW ? 6 : 8, h, sizeof h, &got) == PSA_SUCCESS &&
        got == sizeof h) {
        memcpy(d->serial + 2, h, 4);
        return;
    }
    d->serial[2] = mac[3];
    d->serial[3] = mac[4];
    d->serial[4] = mac[5];
    d->serial[5] = i == GW ? 0 : (uint8_t)(i + 1);
}

static void init_dev(vdev_t *d)
{
    int i = slot(d);
    for (int o = 1; o <= 5; o++) {
        seg_drop(&d->lo[o]);
    }
    memset(d, 0, sizeof *d);
    d->used = true;
    d->ia = IA_UNPROG;
    d->inst = 100 + i;
    d->k = -1;
    d->zaddr = ZADDR_NONE;
    d->zep = 1;
    serial_of(d);
    for (int o = 1; o <= 5; o++) {
        d->lo[o].run_state = 1;
    }
}

// The kept template part a record names: from RAM if another device has it already, else from its blob.
// -1: missing or damaged, -2: no memory.
static int ts_load(uint32_t crc, uint32_t len)
{
    int free_i = -1;
    for (int i = 0; i < NDEV; i++) {
        if (s_ts[i].refs && s_ts[i].crc == crc && s_ts[i].len == len) {
            s_ts[i].refs++;
            return i;
        }
        if (!s_ts[i].refs && free_i < 0) {
            free_i = i;
        }
    }
    char k[16];
    ts_key(crc, len, k);
    size_t n = 0;
    if (free_i < 0 || nvs_get_blob(s_nvs, k, NULL, &n) != ESP_OK || n != len) {
        return -1;
    }
    uint8_t *b = malloc(len);
    if (!b) {
        return -2;
    }
    if (nvs_get_blob(s_nvs, k, b, &n) != ESP_OK || n != len || crc32_of(b, len) != crc) {
        free(b);
        return -1;
    }
    s_ts[free_i] = (tshare_t){.data = b, .len = len, .crc = crc, .refs = 1, .saved = true};
    return free_i;
}

static bool restore(int i)
{
    char key[12];
    rec_key(i, key);
    size_t n = 0;
    if (nvs_get_blob(s_nvs, key, NULL, &n) != ESP_OK || n < REC1_SIZE) {
        return false;
    }
    uint8_t *b = malloc(n);
    if (!b || nvs_get_blob(s_nvs, key, b, &n) != ESP_OK) {
        free(b);
        return false;
    }
    rec_t r = {0};
    size_t head = sizeof r;
    uint32_t magic;
    memcpy(&magic, b, 4);
    bool old = magic == REC_MAGIC_1 || magic == REC_MAGIC_2;
    bool ok = magic == REC_MAGIC_1 || (magic == REC_MAGIC_2 && n >= REC2_SIZE) || (magic == REC_MAGIC && n >= sizeof r);
    if (magic == REC_MAGIC_1) {   // VDV1: the same record without the IEEE address
        head = REC1_SIZE;
        memcpy(&r, b, 12);
        r.flags = 0;
        memcpy(r.o, b + 12, sizeof r.o);
    } else if (magic == REC_MAGIC_2 && ok) {   // VDV2: the whole parameter segment in the record
        head = REC2_SIZE;
        memcpy(&r, b, REC2_SIZE);
    } else if (ok) {
        memcpy(&r, b, sizeof r);
    }
    if (ok && r.tlen) {   // VDV3 with the template part kept apart
        ok = r.o[3].state == LS_LOADED && r.at + r.tlen == r.o[3].size && r.at < r.o[3].size;
        if (ok) {
            s_tref[i].crc = r.tcrc;   // named even if this device does not come back: the blob stays
            s_tref[i].len = r.tlen;
        }
    }
    size_t need = head;
    for (int o = 1; o <= 5 && ok; o++) {
        ok = r.o[o - 1].state <= LS_ERROR && r.o[o - 1].size <= (o == 4 ? BIG4_SIZE : SEG_SIZE);
        need += o == 4 && r.tlen ? r.at : r.o[o - 1].size;
    }
    if (!ok || need != n) {
        ESP_LOGE(TAG, "record %s is damaged (%u B), ignored", key, (unsigned)n);
        free(b);
        return false;
    }
    vdev_t *d = &s_d[i];
    init_dev(d);
    d->ia = r.ia;
    d->zaddr = r.zaddr;
    d->zep = r.zep;
    d->device_control = r.device_control;
    d->k_hint = r.k == 0 || r.k == 1 ? r.k : -1;
    d->have_ieee = r.flags & 1;
    d->gone = r.flags & 2;
    d->cap_known = r.flags & 4;
    d->rx_on = r.flags & 8;
    memcpy(d->ieee, r.ieee, 8);
    serial_of(d);
    const uint8_t *p = b + head;
    bool lost = false;   // the template part the record names is missing or damaged
    for (int o = 1; o <= 5; o++) {
        lobj_t *l = &d->lo[o];
        uint32_t size = r.o[o - 1].size, own = o == 4 && r.tlen ? r.at : size;
        l->state = r.o[o - 1].state;
        l->run_state = r.o[o - 1].run_state;
        l->err = l->state == LS_ERROR ? r.o[o - 1].err : 0;
        memcpy(l->pv, r.o[o - 1].pv, 5);
        if (size && l->state == LS_LOADED) {
            int t = o == 4 && r.tlen ? ts_load(r.tcrc, r.tlen) : -1;
            if (o == 4 && r.tlen && t == -1) {
                lost = true;
                p += own;
                continue;
            }
            l->seg = own ? malloc(own) : NULL;
            if (t == -2 || (own && !l->seg)) {   // half a device would answer with the tables of another object
                ESP_LOGE(TAG, "record %s: no memory for the %u B of object %d, device not restored", key,
                         (unsigned)size, o);
                if (t >= 0) {
                    ts_drop(t);
                }
                for (int q = 1; q <= 5; q++) {
                    seg_drop(&d->lo[q]);
                }
                memset(d, 0, sizeof *d);
                free(b);
                return false;
            }
            if (own) {
                memcpy(l->seg, p, own);
            }
            l->size = size;
            l->cap = own;
            if (t >= 0) {
                l->ts1 = (uint8_t)(t + 1);
                l->at = r.at;
            }
            l->ref = (o == 4 && size > SEG_SIZE) ? BIG4_BASE : SEG_BASE + (uint32_t)(o - 1) * SEG_SIZE;
        }
        p += own;   // an object that is not loaded has no content here; older records may still carry some
    }
    free(b);
    if (lost) {   // the application cannot come back whole: rather nothing loaded, and ETS loads it again
        ESP_LOGE(TAG, "record %s: the template part it names (%u B, CRC %08lx) is missing or damaged; every load "
                 "object unloaded, the address stays", key, (unsigned)r.tlen, (unsigned long)r.tcrc);
        for (int o = 1; o <= 5; o++) {
            seg_drop(&d->lo[o]);
            d->lo[o].state = LS_UNLOADED;
            d->lo[o].err = 0;
            d->lo[o].size = 0;
            d->lo[o].ref = 0;
        }
        mark(d);
        return true;
    }
    seg_share(d);   // a record of before (VDV1, VDV2) or one written while there was no room
    if (old || (d->lo[4].ts1 && !r.tlen)) {
        mark(d);   // written again in the new format
    }
    return true;
}

// ---------------------------------------------------------------- the class behind a device
// built-in class of an application (PID 13: manufacturer, application number, version), -1: none
static int class_of(const uint8_t pv[5])
{
    if (be16(pv) != MANUFACTURER) {
        return -1;
    }
    switch (be16(pv + 2)) {
    case 0xC001:
    case 0xFE20: return 0;
    case 0xC002:
    case 0xFE21: return 1;
    default: return -1;
    }
}

// class of the application ETS loaded, -1: none of ours or nothing loaded
static int app_class(vdev_t *d)
{
    return d->lo[4].state == LS_LOADED ? class_of(d->lo[4].pv) : -1;
}

// The application ETS wrote into PID 13 last, if it is one of the Zigbee block (C000-C1FF, or the earlier test
// numbers FE20/FE21), else 0. Also while its load state is Error: the device info of ETS then names what failed to
// load (2026-11-13: it named C001 for a C003 in Error).
static uint16_t our_app(vdev_t *d)
{
    lobj_t *o = &d->lo[4];
    uint16_t a = be16(o->pv + 2);
    if (be16(o->pv) != MANUFACTURER) {
        return 0;
    }
    return (a >= 0xC000 && a <= 0xC1FF) || a == 0xFE20 || a == 0xFE21 ? a : 0;
}

// order info (PID 15), 10 octets: which kind of device the application makes of it
static const char *order_of(vdev_t *d)
{
    switch (is_gw(d) ? GW_APP : our_app(d)) {
    case GW_APP: return "ZB-GATEWAY";
    case 0xC002:
    case 0xFE21: return CLS[1].order;
    case 0xC003: return "ZB-SWITCH ";
    case 0xC004: return "ZB-REMOTE ";
    default: return CLS[0].order;
    }
}

// hardware type (PID 78) as OpenKNX suggests it: 00 00, application number, version, 00. The application ETS
// wrote if it is one of ours, else the one the device is meant for.
static void hw_type(vdev_t *d, uint8_t out[6])
{
    lobj_t *o = &d->lo[4];
    uint16_t app = is_gw(d) ? GW_APP : class_of(o->pv) == 1 ? 0xC002 : 0xC001;
    uint8_t ver = 1;
    if (our_app(d) && (is_gw(d) ? our_app(d) == GW_APP : our_app(d) != GW_APP)) {
        app = our_app(d);
        ver = o->pv[4];
    }
    out[0] = out[1] = 0;
    out[2] = (uint8_t)(app >> 8);
    out[3] = (uint8_t)app;
    out[4] = ver;
    out[5] = 0;
}

static uint32_t slots_in_use(void)
{
    uint32_t m = 0;   // a built-in class nobody uses goes as well; it comes back when a device needs it (bvm.c)
    for (int i = 0; i < VDEV_MAX; i++) {
        if (s_d[i].used && s_d[i].k >= 0) {
            m |= 1u << s_d[i].k;
        }
    }
    return m;
}

// Is the Zigbee model one of those the downloaded template was made for? ETS does not tell the product at the address
// assignment, so this is the first moment the type can be compared. Reported only: a model the template does not
// list runs with the class defaults.
// 1: listed, 0: not listed, -1: nothing to compare (a built-in class, a template without a list), -2: the device has
// not told its model yet.
static int model_check(vdev_t *d)
{
    char model[40];
    int count = 0, r = d->k >= 2 ? bvm_model_check(d->k, d->inst, model, sizeof model, &count) : -1;
    if (r < 0) {
        return r;
    }
    for (char *c = model; *c; c++) {
        if ((unsigned char)*c < 0x20) {
            *c = '?';
        }
    }
    uint8_t v = r ? 1 : 2;
    if (v != d->model_ok) {
        d->model_ok = v;
        if (r) {
            ESP_LOGI(TAG, "dev %d: Zigbee model '%s' is one of the %d models of template %s", slot(d), model, count,
                     bvm_slot_name(d->k));
        } else {
            ESP_LOGW(TAG, "dev %d: Zigbee model '%s' is NOT among the %d models of template %s; it runs with the "
                     "class defaults", slot(d), model, count, bvm_slot_name(d->k));
            gw_note(GW_NOTE_MODEL, d->ia, 0, model);
        }
    }
    return r;
}

static void send_params(vdev_t *d)
{
    lobj_t *o = &d->lo[4];
    int p = (int)o->size < d->nparams ? (int)o->size : d->nparams;
    p = p > PARAMS_MAX ? PARAMS_MAX : p;
    char h[2 * PARAMS_MAX + 1];
    uint8_t v[PARAMS_MAX];
    seg_read(o, 0, v, (uint32_t)p);
    hex(v, p, h);
    bool ok = bvm_line(d->k, "{\"call\":\"params\",\"inst\":%d,\"id\":%lu,\"p\":\"%s\"}", d->inst, bvm_next_id(), h);
    ESP_LOGI(TAG, "dev %d: %s, parameters %s -> instance %d: %s", slot(d), bvm_slot_name(d->k), h, d->inst,
             ok ? "ok" : "FAILED");
}

static bool alone_in(const vdev_t *d)   // no other device runs the class d runs
{
    for (int i = 0; i < VDEV_MAX; i++) {
        if (&s_d[i] != d && s_d[i].used && s_d[i].k == d->k) {
            return false;
        }
    }
    return true;
}

static void halt(vdev_t *d)   // the device runs no class; a VM nobody uses any more goes
{
    zlink_pause(d->inst);   // its Zigbee device stays its own: linked again when a class runs, off if it was off
    d->k = -1;
    d->model_ok = 0;
    d->reparam = false;
    // What the class said is not the device's state any more: a read gets no answer until a class says it anew
    // (it was answered with the last value of the class that stopped)
    memset(d->ko, 0, sizeof d->ko);
    bvm_keep(slots_in_use());
}

// The application ETS had loaded was unloaded, or a download of it broke off: when the connection ends, or the device
// restarts, with object 4 not loaded again, the class stops, as any KNX device stops an application it does not have.
// Not at the Unload itself: a partial download unloads and loads again within one connection.
static void app_gone(vdev_t *d)
{
    if (!d->app_leaving || d->lo[4].state == LS_LOADED) {
        return;
    }
    d->app_leaving = false;
    if (!is_gw(d) && d->k >= 0) {
        ESP_LOGI(TAG, "dev %d: its application is not loaded any more: the class stops", slot(d));
        halt(d);
    }
}

// The application ETS loaded does not run as loaded: the device runs no class, and its load state reads Error with
// err until the next download or start. ETS shows it in the device info; the gateway tells it as well.
static void fault(vdev_t *d, uint8_t err, const char *why)
{
    uint8_t was = d->fault;
    d->fault = err ? err : KNX_ERR_SOFTWARE;
    halt(d);
    if (d->fault != was) {   // a retry that fails the same way tells nothing new
        gw_note(GW_NOTE_FAULT, d->ia, d->fault, why);
    }
}

// LoadCompleted of the application: would it come up? ETS reads the answer to this load event, so what the gateway
// cannot take shows in ETS as a failed download, not as a device that stays silent after it. The checks of
// apply() up to the VM, without loading one. 0: it would, else KNX_ERR_* and why.
static uint8_t precheck(vdev_t *d, char *why, size_t wn)
{
    lobj_t *o = &d->lo[4];
    bool ours = be16(o->pv) == MANUFACTURER;
    uint16_t app = be16(o->pv + 2);
    d->next_tpl = false;
    why[0] = 0;
    if (s_test_refuse) {
        uint8_t e = s_test_refuse;
        s_test_refuse = 0;
        snprintf(why, wn, "refused for a test (console tplfail %u)", e);
        return e;
    }
    if (is_gw(d)) {
        if (!ours || app != GW_APP) {
            snprintf(why, wn, "application %02x%02x %02x%02x is not the gateway's own (%04x %04x)", o->pv[0],
                     o->pv[1], o->pv[2], o->pv[3], MANUFACTURER, GW_APP);
            return KNX_ERR_CONFIG;
        }
        return gw_params_check(o->seg, o->size, d->ia, why, wn);   // its segment is never kept apart (seg_share)
    }
    if (ours && app == GW_APP) {
        snprintf(why, wn, "application %04x is the gateway's own; it belongs into the gateway", GW_APP);
        return KNX_ERR_CONFIG;
    }
    if (!o->size) {
        return 0;
    }
    uint8_t *flat = o->seg;
    if (o->ts1) {
        flat = malloc(o->size);
        if (!flat) {
            snprintf(why, wn, "no memory (%u B) to check the template", (unsigned)o->size);
            return KNX_ERR_NO_RAM;
        }
        seg_read(o, 0, flat, o->size);
    }
    // the VM of the class this device runs alone goes before the new one loads (apply()): its heap counts as free
    size_t extra = d->k >= 0 && alone_in(d) ? bvm_slot_heap(d->k) : 0;
    kgt_info_t ti;
    int r = bvm_template_check(flat, o->size, extra, &ti, why, wn);
    if (flat != o->seg) {
        free(flat);
    }
    if (r == BVM_REFUSED) {
        return ti.err ? ti.err : KNX_ERR_SOFTWARE;
    }
    if (r == BVM_NO_TEMPLATE) {
        if (class_of(o->pv) < 0) {
            snprintf(why, wn, "application %02x%02x %02x%02x version %u carries no template and is none of this "
                     "firmware's classes", o->pv[0], o->pv[1], o->pv[2], o->pv[3], o->pv[4]);
            return KNX_ERR_CONFIG;
        }
        return 0;
    }
    if (ti.params > PARAMS_MAX) {
        snprintf(why, wn, "the template takes %u parameter octets, %d fit into a request", (unsigned)ti.params,
                 PARAMS_MAX);
        return KNX_ERR_CONFIG;
    }
    d->next_tpl = true;
    snprintf(d->next_digest, sizeof d->next_digest, "%s", ti.digest);
    d->next_params = (uint16_t)ti.params;
    return 0;
}

// After a download and after a start: bring up what the parameter segment says. A template embedded in it wins
// (kgt.c checks it, bvm.c runs it in a VM of its own); without one the class built in for the application number
// runs. A template that fails a check is not loaded and the parameters that came with it are not applied: the
// device runs no class then and its load state reads Error (earlier firmware kept the class it had and ran it with
// the tables of the new download).
static void apply(vdev_t *d)
{
    lobj_t *o = &d->lo[4];
    if (o->state != LS_LOADED || !o->size) {
        return;
    }
    if (is_gw(d)) {   // the gateway's own application runs in the firmware (gw.c), no class and no template
        if (be16(o->pv) != MANUFACTURER || be16(o->pv + 2) != GW_APP) {
            ESP_LOGW(TAG, "gateway: application %02x%02x %02x%02x version %u is not the gateway's own (%04x %04x): "
                     "its parameters are not applied", o->pv[0], o->pv[1], o->pv[2], o->pv[3], o->pv[4], MANUFACTURER,
                     GW_APP);
            d->fault = KNX_ERR_CONFIG;
            gw_note(GW_NOTE_FAULT, d->ia, d->fault, "the gateway's own application is not loaded in it");
            return;
        }
        d->fault = 0;
        gw_params(o->seg, o->size, d->ia);
        return;
    }
    if (d->zaddr == ZADDR_NONE) {   // before anything is loaded: a VM without a user would stay behind
        ESP_LOGW(TAG, "dev %d: no Zigbee device linked, nothing loaded", slot(d));
        return;
    }
    // A device that changes its template and runs its class alone gives that VM back before the new one loads, as
    // the check at LoadCompleted counted it: with both loaded at once the new one was refused for want of memory
    // (2026-11-12).
    if (d->next_tpl && d->k >= 0 && alone_in(d) && strcmp(bvm_slot_digest(d->k), d->next_digest) != 0) {
        ESP_LOGI(TAG, "dev %d: gives up %s (%u B) before its new template loads", slot(d), bvm_slot_name(d->k),
                 (unsigned)bvm_slot_heap(d->k));
        halt(d);
    }
    d->next_tpl = false;
    d->retry_at = 0;
    kgt_info_t ti;
    char why[112];
    int params, k;
    tshare_t *part = o->ts1 ? &s_ts[o->ts1 - 1] : NULL;
    // The template part passed the checks before and its VM runs: the same octets carry the same template, nothing to
    // check again. Every restart (a partial download of the tables included) checked it anew before, with 23 to 43 KB of
    // heap, and a check that found no memory then left a device that ran well without its class.
    int running = part && part->digest[0] ? bvm_slot_of(part->digest) : -1;
    if (running >= 2) {
        k = running;
        memset(&ti, 0, sizeof ti);
        ti.params = part->params;
        snprintf(ti.digest, sizeof ti.digest, "%s", part->digest);
    } else {
        uint8_t *flat = o->seg;   // the check needs the segment in one piece: put a kept template part behind it
        if (o->ts1) {
            flat = malloc(o->size);
            if (!flat) {
                d->tpl = 2;
                ESP_LOGE(TAG, "dev %d: no memory (%u B) to check the template, nothing loaded; tried again in %d s",
                         slot(d), (unsigned)o->size, APPLY_RETRY_US / 1000000);
                fault(d, KNX_ERR_NO_RAM, "no memory to check the template");
                d->retry_at = esp_timer_get_time() + APPLY_RETRY_US;
                return;
            }
            seg_read(o, 0, flat, o->size);
        }
        k = bvm_template(flat, o->size, &ti, why, sizeof why);
        if (flat != o->seg) {
            free(flat);
        }
        if (k == BVM_REFUSED) {
            d->tpl = 2;
            // No memory says nothing about the template: it is tried again, while a refused content waits for ETS. A
            // load that ran out of memory itself only APPLY_OOM_TRIES times: a template that runs the heap dry would do
            // it every 30 s next to Zigbee and the bus (found in a review); a download or a start tries anew
            bool again = ti.err == KNX_ERR_NO_RAM && (!ti.ran_out || ++d->oom_tries < APPLY_OOM_TRIES);
            char after[96] = "";
            if (again) {
                snprintf(after, sizeof after, "; tried again in %d s", APPLY_RETRY_US / 1000000);
            } else if (ti.ran_out) {
                snprintf(after, sizeof after, "; not tried again until a download or a restart (the load ran out of memory "
                         "%u times since)", (unsigned)d->oom_tries);
            }
            ESP_LOGW(TAG, "dev %d: embedded template REFUSED: %s. Nothing loaded, parameters not applied, the device runs "
                     "no class (checked in %u ms)%s", slot(d), why, (unsigned)(ti.us / 1000), after);
            fault(d, ti.err, why);
            if (again) {
                d->retry_at = esp_timer_get_time() + APPLY_RETRY_US;
            }
            return;
        }
        if (k >= 2) {
            tpl_checked(d, ti.digest, (uint16_t)ti.params);
            d->oom_tries = 0;
        }
    }
    bool tpl = k != BVM_NO_TEMPLATE;
    if (!tpl) {
        k = app_class(d);
        if (k < 0) {
            // Neither one of our classes nor a template: the tables now belong to that application, so the class
            // that ran before must not go on with them. The device runs no class until the next download.
            ESP_LOGW(TAG, "dev %d: application %02x%02x %02x%02x version %u carries no template and is none of this "
                     "firmware's classes: the device runs no class", slot(d), o->pv[0], o->pv[1], o->pv[2], o->pv[3],
                     o->pv[4]);
            d->tpl = 0;
            fault(d, KNX_ERR_CONFIG, "its application carries no template and is none of this firmware's classes");
            return;
        }
        params = CLS[k].params;
        ESP_LOGI(TAG, "dev %d: parameter segment of %u B carries no template, built-in class %s", slot(d),
                 (unsigned)o->size, bvm_slot_name(k));
    } else {
        params = (int)ti.params;
        if (params > PARAMS_MAX) {   // the class would get a cut segment: rather nothing
            d->tpl = 2;
            ESP_LOGE(TAG, "dev %d: embedded template %s REFUSED: it takes %d parameter octets, %d fit into a request",
                     slot(d), bvm_slot_name(k), params, PARAMS_MAX);
            fault(d, KNX_ERR_CONFIG, "the template takes more parameter octets than fit into a request");
            return;
        }
        if (running >= 2) {
            ESP_LOGI(TAG, "dev %d: embedded template %s in slot %d: digest %s, checked before and running, not checked "
                     "again", slot(d), bvm_slot_name(k), k, ti.digest);
        } else {
            ESP_LOGI(TAG, "dev %d: embedded template %s in slot %d: digest %s, signed by %s, %u characters -> %u B "
                     "compressed -> %u B, checked in %u ms", slot(d), bvm_slot_name(k), k, ti.digest, ti.fpr,
                     (unsigned)ti.text_len, (unsigned)ti.z, (unsigned)ti.payload, (unsigned)(ti.us / 1000));
        }
    }
    if (zlink_set(d->inst, k, d->zaddr, d->zep) != 0) {
        ESP_LOGE(TAG, "dev %d: linking instance %d failed", slot(d), d->inst);
        fault(d, KNX_ERR_SOFTWARE, "linking the class to the Zigbee device failed");   // a fresh VM may have no user
        return;
    }
    // Unknown (records of earlier development versions, an announce that was lost): a remote is a sleepy end
    // device, the rest keeps its receiver on. The interview's node descriptor tells it later.
    zlink_sleepy(d->inst, d->cap_known ? !d->rx_on : our_app(d) == 0xC004);
    if (d->gone) {   // its Zigbee device left the network: off, and not asked at an address another may have now
        zlink_gone(d->inst, true);
    }
    if (k != d->k) {
        d->model_ok = 0;
    }
    d->fault = 0;
    d->k = (int8_t)k;
    d->tpl = tpl ? 1 : 0;
    d->nparams = (uint8_t)params;
    bvm_keep(slots_in_use());
    send_params(d);
    // In a VM the instance is new to, the model comes after the parameters: the device is asked and answers later.
    // A class may look at the model in params(), so it gets them once more when the model is there.
    d->reparam = tpl && model_check(d) == -2;
    d->reread_at = esp_timer_get_time() + 30000000;
    d->reread_n = 0;
}

// ---------------------------------------------------------------- sending
// Priorities: the transport layer's control frames (T_ACK, T_NAK, T_Disconnect) and the network management on
// broadcast (individual address services) go with "system". Every answer of device management, and every extended
// frame, goes with "low", as the conformance tests expect them (08_03_07 2.6, 2.7: BCh, 3Ch) and as 03_03_02 2.2.3
// makes "low" mandatory for long frames. Group telegrams go with "low" as well.
#define PRIO_SYSTEM 0
#define PRIO_LOW    3

static bool tx(vdev_t *d, uint16_t dst, bool group, uint8_t prio, const uint8_t *tpdu, uint8_t len)
{
    if (!knx_link_send(d->ia, dst, group, prio, tpdu, len)) {
        return false;
    }
    d->tx++;
    return true;
}

static void tx_ctl(vdev_t *d, uint16_t dst, uint8_t tpci)
{
    tx(d, dst, false, PRIO_SYSTEM, &tpci, 1);
}

static void kick(vdev_t *d)
{
    conn_t *c = &d->c;
    if (!c->on || !c->qc) {
        return;
    }
    uint8_t t[KNX_TPDU_MAX];
    memcpy(t, c->q[c->qh], c->qn[c->qh]);
    t[0] = (uint8_t)(0x40 | c->tx_seq << 2 | (t[0] & 0x03));   // T_Data_Connected
    tx(d, c->remote, false, PRIO_LOW, t, c->qn[c->qh]);
    c->sent = c->last = esp_timer_get_time();   // an answer on its way keeps the connection: three repetitions, then the end
}

static void conn_drop(vdev_t *d, const char *why)
{
    conn_t *c = &d->c;
    if (!c->on) {
        return;
    }
    tx_ctl(d, c->remote, 0x81);   // T_Disconnect
    c->on = false;
    ESP_LOGI(TAG, "dev %d: connection closed (%s)", slot(d), why);
    app_gone(d);
}

typedef struct {
    vdev_t *d;
    uint16_t to;
    bool connected;
} rctx_t;

// a is an APDU: a[0] carries the two high APCI bits, the TPCI bits are filled in on the way out
static void reply(const rctx_t *r, uint8_t *a, int n)
{
    if (n < 2 || n > KNX_TPDU_MAX) {
        return;
    }
    if (!r->connected) {
        a[0] &= 0x03;   // T_Data_Individual
        tx(r->d, r->to, false, PRIO_LOW, a, (uint8_t)n);
        return;
    }
    conn_t *c = &r->d->c;
    if (!c->on || c->qc >= 4) {
        ESP_LOGW(TAG, "dev %d: answer dropped (%s)", slot(r->d), c->on ? "queue full" : "no connection");
        return;
    }
    int i = (c->qh + c->qc) % 4;
    memcpy(c->q[i], a, (size_t)n);
    c->qn[i] = (uint8_t)n;
    if (++c->qc == 1) {
        kick(r->d);
    }
}

static uint8_t *apci(uint8_t *b, uint16_t code)
{
    b[0] = (uint8_t)(code >> 8);
    b[1] = (uint8_t)code;
    return b + 2;
}

// ---------------------------------------------------------------- load state machines and segments
static uint32_t seg_total(void)   // a kept template part counts once, however many devices share it
{
    uint32_t t = 0;
    for (int i = 0; i < NDEV; i++) {
        for (int o = 1; o <= 5 && s_d[i].used; o++) {
            t += s_d[i].lo[o].cap;
        }
        t += s_ts[i].refs ? s_ts[i].len : 0;
    }
    return t;
}

// The buffer follows the allocation: asking for less gives memory back, and all segments together stay within
// SEG_BUDGET, so the bus cannot take the heap away from Zigbee and Berry. The content survives Unload and a new
// allocation of the same size without fill, which is what a partial download relies on.
// 0, or why not (KNX_ERR_*, the error code of the load state).
static uint8_t seg_alloc(vdev_t *d, int obj, uint32_t size, bool fill, uint8_t value)
{
    lobj_t *o = &d->lo[obj];
    if (size == 0) {
        return KNX_ERR_ALLOC_0;
    }
    if (size > (obj == 4 ? BIG4_SIZE : SEG_SIZE)) {
        return KNX_ERR_TOO_LONG;
    }
    if (o->ts1 && (seg_total() + (s_ts[o->ts1 - 1].refs > 1 ? s_ts[o->ts1 - 1].len : 0) > SEG_BUDGET || !seg_own(o))) {
        ESP_LOGW(TAG, "dev %d: object %d cannot take back its template part for the download: refused", slot(d), obj);
        return KNX_ERR_NO_RAM;   // a download writes into its own copy; what it does not write must still be there
    }
    if (o->cap != size) {
        if (seg_total() - o->cap + size > SEG_BUDGET) {
            ESP_LOGW(TAG, "dev %d: object %d asks for %u B, all segments together would pass %u B: refused", slot(d),
                     obj, (unsigned)size, (unsigned)SEG_BUDGET);
            return KNX_ERR_NO_RAM;
        }
        uint8_t *p = realloc(o->seg, size);
        if (!p) {
            return KNX_ERR_NO_RAM;
        }
        if (size > o->cap) {
            memset(p + o->cap, 0, size - o->cap);
        }
        o->seg = p;
        o->cap = size;
    }
    if (fill) {
        memset(o->seg, value, size);
    }
    o->keep_until = 0;
    o->size = size;
    o->ref = (obj == 4 && size > SEG_SIZE) ? BIG4_BASE : SEG_BASE + (uint32_t)(obj - 1) * SEG_SIZE;
    return 0;
}

static void load_event(vdev_t *d, int obj, const uint8_t *data, int n)
{
    lobj_t *o = &d->lo[obj];
    uint8_t before = o->state, ev = n ? data[0] : 0, err = 0;
    char why[112] = "";
    if (obj == 4 && (ev == LE_START || ev == LE_UNLOAD) && before == LS_LOADED) {
        d->app_leaving = true;   // the class stops if the application is not loaded again (app_gone())
    }
    if (ev == LE_START) {   // 03_05_01 Table 94: Error stays Error until Unload
        if (o->state != LS_ERROR) {
            o->state = LS_LOADING;
            o->ref = 0;
            o->size = 0;
        }
    } else if (ev == LE_COMPLETE) {
        if (o->state == LS_LOADING) {
            err = obj == 4 ? precheck(d, why, sizeof why) : 0;   // ETS sees a refusal in the answer to this event
            if (!err) {
                o->state = LS_LOADED;
                if (obj == 4) {
                    d->fault = 0;   // what this download brings is checked; apply() at the restart decides anew
                    d->retry_at = 0;
                    d->oom_tries = 0;
                    d->app_leaving = false;
                    seg_share(d);
                    tpl_checked(d, d->next_tpl ? d->next_digest : NULL, d->next_params);
                }
            }
        }
    } else if (ev == LE_UNLOAD) {
        o->state = LS_UNLOADED;
        o->ref = 0;
        o->size = 0;
        if (obj == 4) {
            d->fault = 0;
        }
        // A partial download unloads, loads and allocates again within milliseconds and relies on the content.
        // Nobody else needs it: after a minute the buffer goes back to the heap.
        o->keep_until = esp_timer_get_time() + SEG_KEEP_US;
    } else if (ev == LE_ADDITIONAL) {
        // System B knows only the data relative allocation 0Bh: size (4), mode (bit 0: fill), fill octet
        if (o->state != LS_LOADING || n < 8 || data[1] != 0x0B) {
            err = KNX_ERR_LOAD_CMD;
            snprintf(why, sizeof why, "load control %02x while %s: only a relative data allocation (0b) while "
                     "loading", n > 1 ? data[1] : 0, o->state == LS_LOADING ? "loading" : "not loading");
        } else if ((err = seg_alloc(d, obj, be32(data + 2), data[6] & 1, data[7])) != 0) {
            snprintf(why, sizeof why, "allocation of %u B for object %d refused", (unsigned)be32(data + 2), obj);
        }
    }
    if (err) {   // the first error is the one PID 28 keeps (03_05_01 4.2.28)
        if (o->state != LS_ERROR) {
            o->err = err;
            o->keep_until = esp_timer_get_time() + SEG_KEEP_US;   // nothing reads it in Error: its RAM goes back
        }
        o->state = LS_ERROR;
    } else if (o->state != LS_ERROR) {
        o->err = 0;
    }
    char h[21];
    hex(data, n > 10 ? 10 : n, h);
    ESP_LOGI(TAG, "dev %d: lsm obj %d event %s -> state %u, ref 0x%04x, size %u", slot(d), obj, h, o->state,
             (unsigned)o->ref, (unsigned)o->size);
    if (o->state == LS_ERROR && before != LS_ERROR) {
        char a[12];
        ia_str(d->ia, a);
        ESP_LOGW(TAG, "dev %d (%s): load object %d in Error, code %u: %s", slot(d), a, obj, o->err, why);
        gw_note(GW_NOTE_REFUSED, d->ia, o->err, why);
        if (obj == 4 && !is_gw(d)) {
            halt(d);   // as any KNX device: an application that is not loaded does not run
        }
    }
    if (o->state != before) {
        mark(d);
        if (obj <= 3) {
            publish();
        }
    }
}

static lobj_t *seg_at(vdev_t *d, uint32_t addr, uint32_t n, bool loading_only)
{
    for (int i = 1; i <= 5; i++) {
        lobj_t *o = &d->lo[i];
        if (o->size && o->ref <= addr && addr + n <= o->ref + o->size) {
            bool ok = loading_only ? o->state == LS_LOADING : (o->state == LS_LOADING || o->state == LS_LOADED);
            return ok ? o : NULL;
        }
    }
    return NULL;
}

// ---------------------------------------------------------------- interface objects
#define PDT_CONTROL       0x00
#define PDT_UNSIGNED_CHAR 0x02
#define PDT_UNSIGNED_INT  0x04
#define PDT_UNSIGNED_LONG 0x09
#define PDT_GENERIC_01    0x11
#define PDT_GENERIC_02    0x12
#define PDT_GENERIC_04    0x14
#define PDT_GENERIC_05    0x15
#define PDT_GENERIC_06    0x16
#define PDT_GENERIC_08    0x18
#define PDT_GENERIC_10    0x1A
#define PDT_VERSION       0x30
#define PDT_BITSET8       0x33
#define WR                0x80

typedef struct {
    uint8_t pid, type;   // type: PDT, | WR if writable
} prop_t;

static const prop_t P_DEV[] = {{1, PDT_UNSIGNED_INT}, {11, PDT_GENERIC_06}, {12, PDT_UNSIGNED_INT},
                               {14, PDT_GENERIC_01 | WR}, {15, PDT_GENERIC_10}, {25, PDT_VERSION},
                               {54, PDT_BITSET8 | WR}, {56, PDT_UNSIGNED_INT}, {57, PDT_UNSIGNED_CHAR},
                               {58, PDT_UNSIGNED_CHAR}, {78, PDT_GENERIC_06}, {83, PDT_GENERIC_02}};
// PID_TABLE (23) is mandatory in both table objects: ETS reads its description to learn the table format
// PID_ERROR_CODE (28) last: ETS may ask for descriptions by index, which stay what they were. PDT_UNSIGNED_CHAR is
// the alternative 03_05_01 4.2.28 allows for PDT_ENUM8.
static const prop_t P_ADDR[] = {{1, PDT_UNSIGNED_INT}, {5, PDT_CONTROL | WR}, {7, PDT_UNSIGNED_LONG},
                                {23, PDT_GENERIC_02}, {27, PDT_GENERIC_08}, {28, PDT_UNSIGNED_CHAR}};
static const prop_t P_ASSOC[] = {{1, PDT_UNSIGNED_INT}, {5, PDT_CONTROL | WR}, {7, PDT_UNSIGNED_LONG},
                                 {23, PDT_GENERIC_04}, {27, PDT_GENERIC_08}, {28, PDT_UNSIGNED_CHAR}};
static const prop_t P_GO[] = {{1, PDT_UNSIGNED_INT}, {5, PDT_CONTROL | WR}, {7, PDT_UNSIGNED_LONG},
                              {27, PDT_GENERIC_08}, {28, PDT_UNSIGNED_CHAR}};
static const prop_t P_PROG[] = {{1, PDT_UNSIGNED_INT}, {5, PDT_CONTROL | WR}, {6, PDT_CONTROL | WR},
                                {7, PDT_UNSIGNED_LONG}, {13, PDT_GENERIC_05 | WR}, {27, PDT_GENERIC_08},
                                {28, PDT_UNSIGNED_CHAR}};

static const prop_t *props(int obj, int *n)
{
    switch (obj) {
    case 0: *n = sizeof P_DEV / sizeof *P_DEV; return P_DEV;
    case 1: *n = sizeof P_ADDR / sizeof *P_ADDR; return P_ADDR;
    case 2: *n = sizeof P_ASSOC / sizeof *P_ASSOC; return P_ASSOC;
    case 3: *n = sizeof P_GO / sizeof *P_GO; return P_GO;
    case 4:
    case 5: *n = sizeof P_PROG / sizeof *P_PROG; return P_PROG;
    default: *n = 0; return NULL;
    }
}

static const prop_t *prop(int obj, int pid, int *idx)
{
    int n;
    const prop_t *t = props(obj, &n);
    for (int i = 0; i < n; i++) {
        if (t[i].pid == pid) {
            if (idx) {
                *idx = i;
            }
            return &t[i];
        }
    }
    return NULL;
}

static int prop_read(vdev_t *d, int obj, int pid, uint8_t *out)   // octets written, -1: no such property
{
    static const uint8_t OBJ_TYPE[] = {0, 1, 2, 9, 3, 4};   // device, address table, association table, group
    if (!prop(obj, pid, NULL)) {                              // object table, application, PEI program
        return -1;
    }
    if (pid == 1) {
        out[0] = 0;
        out[1] = OBJ_TYPE[obj];
        return 2;
    }
    if (obj == 0) {
        switch (pid) {
        case 11: memcpy(out, d->serial, 6); return 6;
        case 12: out[0] = MANUFACTURER >> 8; out[1] = MANUFACTURER & 0xFF; return 2;
        case 14: out[0] = d->device_control; return 1;
        case 15:   // order info: what the application is
            memcpy(out, order_of(d), 10);
            return 10;
        case 25: out[0] = 0; out[1] = 1; return 2;
        case 54: out[0] = d->prog ? 1 : 0; return 1;
        case 56: out[0] = 0; out[1] = s_max_apdu; return 2;
        case 57: out[0] = (uint8_t)(d->ia >> 8); return 1;
        case 58: out[0] = (uint8_t)d->ia; return 1;
        case 78: hw_type(d, out); return 6;
        case 83: out[0] = MASK >> 8; out[1] = MASK & 0xFF; return 2;
        }
        return -1;
    }
    lobj_t *o = &d->lo[obj];
    bool faulty = obj == 4 && d->fault && o->state == LS_LOADED;   // loaded, but it does not run as loaded
    switch (pid) {
    case 5: out[0] = faulty ? LS_ERROR : o->state; return 1;
    // 0: Halted, "halted or not loaded" (03_05_01 Table 95); ETS shows it in the device info as "Gestoppt"
    case 6: out[0] = faulty || o->state != LS_LOADED ? 0 : o->run_state; return 1;
    case 28: out[0] = o->state == LS_ERROR ? o->err : faulty ? d->fault : 0; return 1;
    case 7: {
        uint32_t r = o->state != LS_UNLOADED ? o->ref : 0;
        out[0] = (uint8_t)(r >> 24); out[1] = (uint8_t)(r >> 16); out[2] = (uint8_t)(r >> 8); out[3] = (uint8_t)r;
        return 4;
    }
    case 13: memcpy(out, o->pv, 5); return 5;
    case 27: {   // size, 00 FF, CRC-16-CCITT over the segment: ETS compares it before a partial download
        uint16_t c = seg_crc(o);
        out[0] = (uint8_t)(o->size >> 24); out[1] = (uint8_t)(o->size >> 16); out[2] = (uint8_t)(o->size >> 8);
        out[3] = (uint8_t)o->size; out[4] = 0x00; out[5] = 0xFF; out[6] = (uint8_t)(c >> 8); out[7] = (uint8_t)c;
        return 8;
    }
    }
    return -1;
}

static int prop_write(vdev_t *d, int obj, int pid, const uint8_t *data, int n, uint8_t *out)
{
    const prop_t *p = prop(obj, pid, NULL);
    if (!p || !(p->type & WR) || n < 1) {
        return -1;
    }
    if (obj == 0 && pid == 14) {
        d->device_control = data[0];
    } else if (obj == 0 && pid == 54) {
        set_prog(d, data[0] & 1);
        ESP_LOGI(TAG, "dev %d: programming mode %s (property)", slot(d), d->prog ? "on" : "off");
        publish();
    } else if (pid == 5) {
        load_event(d, obj, data, n);
    } else if (pid == 6) {
        d->lo[obj].run_state = data[0];
    } else if (pid == 13) {
        uint8_t pv[5] = {0};
        memcpy(pv, data, n > 5 ? 5 : (size_t)n);
        if (obj == 4 && !is_gw(d) && memcmp(pv, d->lo[4].pv, 5) != 0) {
            // another application: what an object held under the old one is no value of the new one (a read was
            // answered with it until the new class set it, or for good while its Zigbee device was off)
            memset(d->ko, 0, sizeof d->ko);
        }
        memcpy(d->lo[obj].pv, pv, 5);
        mark(d);
    }
    return prop_read(d, obj, pid, out);
}

// ---------------------------------------------------------------- application layer, individually addressed
static void hold(vdev_t *d, int ko);

static void restart(vdev_t *d)
{
    const uint8_t *g, *a;
    int ng = ga_count(d, &g), na = assoc_count(d, &a);
    ESP_LOGI(TAG, "dev %d: restart; %d group addresses, %d associations, parameter segment %u B, %lu memory writes "
             "with %lu octets (%lu refused)",
             slot(d), ng, na, (unsigned)(d->lo[4].state == LS_LOADED ? d->lo[4].size : 0),
             (unsigned long)d->mem_wr, (unsigned long)d->mem_wr_octets, (unsigned long)d->mem_rejected);
    d->mem_wr = d->mem_wr_octets = d->mem_rejected = 0;
    d->oom_tries = 0;   // a restart tries a load that ran out of memory anew (APPLY_OOM_TRIES)
    set_prog(d, false);
    d->c.on = false;
    publish();
    // What its status objects hold goes on the bus once more (tell_held): a download may have given them other group
    // addresses, and a real device tells its state after a restart. A class that changes with this restart starts with
    // nothing held (halt()); its first values are held when they come.
    for (int ko = 1; ko <= KO_MAX && !is_gw(d); ko++) {
        d->ko[ko].told = 0;
        if (d->ko[ko].have && d->ko[ko].status) {
            hold(d, ko);
        }
    }
    apply(d);
    app_gone(d);
}

static void apdu(vdev_t *d, uint16_t src, const uint8_t *t, int len, bool connected)
{
    if (len < 2) {
        return;
    }
    const rctx_t r = {.d = d, .to = src, .connected = connected};
    uint16_t a = (uint16_t)((t[0] & 0x03) << 8 | t[1]);
    uint8_t b[KNX_TPDU_MAX + 4], *p;   // the longest answer: a memory read or echo of 63 octets, 67 in all
    if ((a & 0x3C0) == 0x300) {   // A_DeviceDescriptor_Read
        int type = a & 0x3F;
        if (type == 0) {
            p = apci(b, 0x340);
            *p++ = MASK >> 8;
            *p++ = MASK & 0xFF;
        } else {
            p = apci(b, 0x340 | 0x3F);   // descriptor type not supported
        }
        ESP_LOGI(TAG, "dev %d: device descriptor type %d read", slot(d), type);
        reply(&r, b, (int)(p - b));
    } else if (a == 0x3D1) {   // A_Authorize_Request: every key gets level 0
        p = apci(b, 0x3D2);
        *p++ = 0;
        ESP_LOGI(TAG, "dev %d: authorize -> level 0", slot(d));
        reply(&r, b, (int)(p - b));
    } else if (a == 0x3D5 && len >= 6) {   // A_PropertyValue_Read
        int obj = t[2], pid = t[3], count = t[4] >> 4, start = (t[4] & 0x0F) << 8 | t[5];
        uint8_t v[PROP_MAX];
        int n = -1, rc = 0;
        if (pid == 23 && (obj == 1 || obj == 2)) {   // PID_TABLE: the table itself as an array
            const uint8_t *tab;
            int es = obj == 1 ? 2 : 4, cnt = obj == 1 ? ga_count(d, &tab) : assoc_count(d, &tab);
            if (start == 0) {
                v[0] = (uint8_t)(cnt >> 8);
                v[1] = (uint8_t)cnt;
                n = 2;
                rc = 1;
            } else if (count && start + count - 1 <= cnt && count * es <= s_max_apdu - 5) {   // what the answer takes
                memcpy(v, tab + 2 + (start - 1) * es, (size_t)(count * es));
                n = count * es;
                rc = count;
            }
        } else {
            n = prop_read(d, obj, pid, v);
            if (n >= 0 && start == 0) {   // current number of elements
                v[0] = 0;
                v[1] = 1;
                n = 2;
                rc = 1;
            } else if (n >= 0 && start == 1 && count == 1) {
                rc = 1;
            } else {
                n = -1;
            }
        }
        p = apci(b, 0x3D6);
        *p++ = (uint8_t)obj;
        *p++ = (uint8_t)pid;
        *p++ = (uint8_t)((n < 0 ? 0 : rc) << 4 | start >> 8);
        *p++ = (uint8_t)start;
        if (n > 0) {
            memcpy(p, v, (size_t)n);
            p += n;
        }
        char vh[2 * PROP_MAX + 1] = "absent";
        if (n >= 0) {
            hex(v, n, vh);
        }
        ESP_LOGI(TAG, "dev %d: read obj %d pid %d, %d from %d -> %s", slot(d), obj, pid, count, start, vh);
        reply(&r, b, (int)(p - b));
    } else if (a == 0x3D7 && len >= 6) {   // A_PropertyValue_Write
        int obj = t[2], pid = t[3], count = t[4] >> 4, start = (t[4] & 0x0F) << 8 | t[5];
        uint8_t v[12];
        int n = (count == 1 && start == 1) ? prop_write(d, obj, pid, t + 6, len - 6, v) : -1;
        p = apci(b, 0x3D6);
        *p++ = (uint8_t)obj;
        *p++ = (uint8_t)pid;
        *p++ = (uint8_t)((n < 0 ? 0 : 1) << 4 | start >> 8);
        *p++ = (uint8_t)start;
        if (n > 0) {
            memcpy(p, v, (size_t)n);
            p += n;
        }
        if (pid != 5) {   // load events have their own line
            char dh[21], vh[25] = "refused";
            hex(t + 6, len - 6 > 10 ? 10 : len - 6, dh);
            if (n >= 0) {
                hex(v, n, vh);
            }
            ESP_LOGI(TAG, "dev %d: write obj %d pid %d = %s -> %s", slot(d), obj, pid, dh, vh);
        }
        reply(&r, b, (int)(p - b));
    } else if (a == 0x3D8 && len >= 5) {   // A_PropertyDescription_Read
        int obj = t[2], pid = t[3], idx = t[4], n;
        const prop_t *tab = props(obj, &n), *e = NULL;
        if (pid) {
            e = prop(obj, pid, &idx);
        } else if (idx < n) {
            e = &tab[idx];
        }
        p = apci(b, 0x3D9);
        *p++ = (uint8_t)obj;
        if (e) {
            int max = (e->pid == 23) ? 255 : 1;
            *p++ = e->pid;
            *p++ = (uint8_t)idx;
            *p++ = e->type;
            *p++ = (uint8_t)(max >> 8);
            *p++ = (uint8_t)max;
            *p++ = 0x00;   // read and write level 0
        } else {
            *p++ = (uint8_t)pid;
            *p++ = (uint8_t)idx;
            *p++ = 0;
            *p++ = 0;
            *p++ = 0;
            *p++ = 0;
        }
        ESP_LOGI(TAG, "dev %d: description of obj %d pid %d index %d -> %s", slot(d), obj, pid, idx,
                 e ? "known" : "absent");
        reply(&r, b, (int)(p - b));
    } else if ((a == 0x2C7 || a == 0x2C8) && len >= 4) {   // function property: none here (03_03_07 3.4.7.3)
        p = apci(b, 0x2C9);
        *p++ = t[2];
        *p++ = t[3];
        ESP_LOGI(TAG, "dev %d: function property obj %d pid %d -> none", slot(d), t[2], t[3]);
        reply(&r, b, (int)(p - b));
    } else if ((a & 0x3C0) == 0x200 && len >= 4) {   // A_Memory_Read
        int count = a & 0x3F;
        uint32_t addr = be16(t + 2);
        lobj_t *o = count && count <= s_max_apdu - 3 ? seg_at(d, addr, (uint32_t)count, false) : NULL;
        p = apci(b, (uint16_t)(0x240 | (o ? count : 0)));
        *p++ = t[2];
        *p++ = t[3];
        if (o) {
            seg_read(o, addr - o->ref, p, (uint32_t)count);
            p += count;
        } else {
            d->mem_rejected++;
        }
        ESP_LOGI(TAG, "dev %d: memory read, %d octets at 0x%04x%s", slot(d), count, (unsigned)addr,
                 o ? "" : " refused");
        reply(&r, b, (int)(p - b));
    } else if ((a & 0x3C0) == 0x280 && len >= 4) {   // A_Memory_Write
        int count = a & 0x3F;
        uint32_t addr = be16(t + 2);
        // Nothing is written when number exceeds PID 56 - 3 or is not the number of data octets that came (03_03_07
        // p. 113); verify mode answers that with number 0, as any write that fails (08_03_07 2.7.11)
        bool fits = count && count <= s_max_apdu - 3 && len == 4 + count;
        lobj_t *o = fits ? seg_at(d, addr, (uint32_t)count, true) : NULL;
        if (o && !seg_own(o)) {   // never while a part is kept apart (an allocation came first), but never into it
            o = NULL;
        }
        if (o) {
            memcpy(o->seg + (addr - o->ref), t + 4, (size_t)count);
            d->mem_wr++;
            d->mem_wr_octets += (uint32_t)count;
            mark(d);
        } else {
            d->mem_rejected++;
            ESP_LOGW(TAG, "dev %d: memory write of %d octets at 0x%04x refused", slot(d), count, (unsigned)addr);
        }
        if (d->device_control & 0x04) {   // verify mode: echo, or count 0 on error
            // The echo is as long as the write; up to 63 octets (6-bit count) fit an answer of KNX_TPDU_MAX
            p = apci(b, (uint16_t)(0x240 | (o ? count : 0)));
            *p++ = t[2];
            *p++ = t[3];
            if (o) {
                seg_read(o, addr - o->ref, p, (uint32_t)count);
                p += count;
            }
            reply(&r, b, (int)(p - b));
        }
    } else if ((a & 0x3C0) == 0x180 && len >= 3) {   // A_ADC_Read
        // Nothing to measure: the NCN5130 tells only whether the bus voltage is above its undervoltage level (VBUS
        // bit, datasheet p. 37). Read count 0 is the answer the standard gives for a channel that cannot be read
        // (03_03_07 p. 107). Earlier firmware answered with a placeholder 0x0D00, which ETS showed as 62.4 V.
        p = apci(b, (uint16_t)(0x1C0 | (a & 0x3F)));
        *p++ = 0;
        *p++ = 0;
        *p++ = 0;
        ESP_LOGI(TAG, "dev %d: ADC channel %d read, %d samples -> none (count 0)", slot(d), a & 0x3F, t[2]);
        reply(&r, b, (int)(p - b));
    } else if ((a & 0x3C0) == 0x380) {   // A_Restart (03_03_07 p. 52, 03_05_02 3.7)
        if (a & 0x3E) {
            // the response bit (an answer of another device) or a reserved bit set: ignored (03_05_02 3.7.2 (1))
            d->unhandled++;
            ESP_LOGW(TAG, "dev %d: A_Restart with APCI 0x%03x ignored", slot(d), a);
        } else if (!(a & 1)) {
            restart(d);   // basic restart, not confirmed
        } else if (len >= 4) {
            // Master reset. Only the confirmed restart is taken (erase code 01h, channel 00h): error code 0 and process
            // time 0 go out before the basic restart it stands for (03_05_02 Table 4). Every other erase code would
            // reset address, application or tables of a device a Zigbee device stands behind: "unsupported erase
            // code" (02h), nothing reset. Erase code 01h with a channel: "invalid channel number" (03h).
            uint8_t err = t[2] != 0x01 ? 0x02 : t[3] ? 0x03 : 0x00;
            p = apci(b, 0x3A1);   // A_Restart_Response, restart type 1
            *p++ = err;
            *p++ = 0;
            *p++ = 0;
            ESP_LOGI(TAG, "dev %d: master reset, erase code %u channel %u -> error code %u", slot(d), t[2], t[3], err);
            reply(&r, b, (int)(p - b));
            if (!err) {
                restart(d);
            }
        }
    } else {
        d->unhandled++;
        char h[33];
        hex(t, len > 16 ? 16 : len, h);
        ESP_LOGW(TAG, "dev %d: unhandled service, APCI 0x%03x, TPDU %s", slot(d), a, h);
    }
}

static void individual(vdev_t *d, const knx_frame_t *f)
{
    uint8_t tp = f->tpdu[0];
    uint16_t src = f->src;
    conn_t *c = &d->c;
    int64_t now = esp_timer_get_time();
    if (tp == 0x80) {   // T_Connect; the same partner again starts over (more forgiving than style 3)
        if (!c->on || c->remote == src) {
            memset(c, 0, sizeof *c);
            c->on = true;
            c->remote = src;
            c->last = now;
            char s[12];
            ia_str(src, s);
            ESP_LOGI(TAG, "dev %d: connected by %s", slot(d), s);
        } else {
            tx_ctl(d, src, 0x81);
        }
    } else if (tp == 0x81) {   // T_Disconnect
        if (c->on && c->remote == src) {
            c->on = false;
            ESP_LOGI(TAG, "dev %d: disconnected by its partner", slot(d));
            app_gone(d);
        }
    } else if ((tp & 0xC0) == 0x40) {   // T_Data_Connected
        if (!c->on || c->remote != src) {
            tx_ctl(d, src, 0x81);
            return;
        }
        c->last = now;
        uint8_t seq = (tp >> 2) & 0x0F;
        if (seq == c->rx_seq) {
            tx_ctl(d, src, (uint8_t)(0xC2 | seq << 2));
            c->rx_seq = (seq + 1) & 15;
            apdu(d, src, f->tpdu, f->len, true);
        } else if (seq == ((c->rx_seq - 1) & 15)) {
            tx_ctl(d, src, (uint8_t)(0xC2 | seq << 2));   // repeated: acknowledge again, do not run it twice
        } else {
            tx_ctl(d, src, (uint8_t)(0xC3 | seq << 2));
        }
    } else if ((tp & 0xC3) == 0xC2) {   // T_ACK
        uint8_t seq = (tp >> 2) & 0x0F;
        if (c->on && c->remote == src && c->qc && seq == c->tx_seq) {
            c->qh = (c->qh + 1) % 4;
            c->qc--;
            c->tx_seq = (c->tx_seq + 1) & 15;
            c->retries = 0;
            c->sent = 0;
            c->last = now;
            kick(d);
        } else if (c->on && c->remote == src) {
            conn_drop(d, "T_ACK with a wrong sequence number");
        }
    } else if ((tp & 0xC3) == 0xC3) {   // T_NAK
        uint8_t seq = (tp >> 2) & 0x0F;
        if (c->on && c->remote == src && c->qc && seq == c->tx_seq) {
            if (++c->retries > 3) {
                conn_drop(d, "T_NAK repeated");
            } else {
                kick(d);
            }
        } else if (c->on && c->remote == src) {
            conn_drop(d, "T_NAK with a wrong sequence number");
        }
    } else if ((tp & 0xFC) == 0x00) {   // T_Data_Individual: connectionless management
        apdu(d, src, f->tpdu, f->len, false);
    }
}

// ---------------------------------------------------------------- broadcast and group communication
// ETS wrote an individual address. 15.15.255 is how it unloads one: for a Zigbee device that means it is to go
// (pair.c removes the virtual device and asks its Zigbee device to leave); the gateway itself just has none then.
static void ia_written(vdev_t *d, uint16_t old)
{
    if (is_gw(d)) {
        gw_ia(d->ia);
    } else if (d->ia == IA_UNPROG) {
        pair_knx_unloaded(slot(d), old);
    } else {
        pair_knx_assigned(slot(d), d->ia);
    }
}

// Another of our devices, the gateway included, has this individual address already: frames to it would reach only the
// first of them (dispatch()). 15.15.255 is no address and may be shared. Every device in programming mode takes an
// address written to it, so two of them would take the same one; ETS writes only when exactly one answered the
// address read (03_05_02 2.3), other tools may not.
static vdev_t *ia_owner(uint16_t ia, const vdev_t *except)
{
    for (int i = 0; i < NDEV && ia != IA_UNPROG; i++) {
        if (s_d[i].used && &s_d[i] != except && s_d[i].ia == ia) {
            return &s_d[i];
        }
    }
    return NULL;
}

static bool ia_free_for(vdev_t *d, uint16_t ia)   // false: refused, and told on the gateway's message object
{
    vdev_t *o = ia_owner(ia, d);
    if (!o) {
        return true;
    }
    char a[12], why[96];
    ia_str(ia, a);
    snprintf(why, sizeof why, "address %s for device %d refused: %s has it already", a, slot(d),
             is_gw(o) ? "the gateway" : "another virtual device");
    ESP_LOGW(TAG, "%s (device %d)", why, slot(o));
    gw_note(GW_NOTE_DUP, ia, 0, why);
    return false;
}

static void broadcast(vdev_t *d, const knx_frame_t *f)
{
    const uint8_t *t = f->tpdu;
    uint16_t a = (uint16_t)((t[0] & 0x03) << 8 | t[1]);
    uint8_t b[14], *p;
    if (a == 0x100 && d->prog) {   // A_IndividualAddress_Read: the source address of the answer is the answer
        s_hit = true;
        p = apci(b, 0x140);
        tx(d, 0, true, PRIO_SYSTEM, b, (uint8_t)(p - b));
    } else if (a == 0x0C0 && d->prog && f->len >= 4) {   // A_IndividualAddress_Write
        char o[12], n[12];
        uint16_t old = d->ia;
        s_hit = true;
        if (!ia_free_for(d, be16(t + 2))) {
            return;
        }
        ia_str(d->ia, o);
        d->ia = be16(t + 2);
        ia_str(d->ia, n);
        mark(d);
        s_hit = true;
        ESP_LOGI(TAG, "dev %d: individual address %s -> %s", slot(d), o, n);
        publish();
        ia_written(d, old);
    } else if (a == 0x3DC && f->len >= 8 && memcmp(t + 2, d->serial, 6) == 0) {   // ..SerialNumber_Read
        p = apci(b, 0x3DD);
        memcpy(p, d->serial, 6);
        p += 6;
        memset(p, 0, 4);   // domain address, reserved
        p += 4;
        tx(d, 0, true, PRIO_SYSTEM, b, (uint8_t)(p - b));
    } else if (a == 0x3DE && f->len >= 10 && memcmp(t + 2, d->serial, 6) == 0) {   // ..SerialNumber_Write
        char n[12];
        uint16_t old = d->ia;
        s_hit = true;
        if (!ia_free_for(d, be16(t + 8))) {
            return;
        }
        d->ia = be16(t + 8);
        ia_str(d->ia, n);
        mark(d);
        s_hit = true;
        ESP_LOGI(TAG, "dev %d: individual address %s by serial number", slot(d), n);
        publish();
        ia_written(d, old);
    }
}

static int ko_tpdu(vdev_t *d, int asap, uint16_t code, uint8_t *t)   // TPDU octets, 0: value does not fit
{
    int n = go_octets(descriptor(d, asap) & 0xFF);
    int have = d->ko[asap].len;
    const uint8_t *v = d->ko[asap].v;
    if (n > KNX_KO_VAL) {
        return 0;
    }
    t[0] = (uint8_t)(code >> 8);
    t[1] = (uint8_t)code;
    if (n == 0) {
        t[1] |= (have ? v[have - 1] : 0) & 0x3F;
        return 2;
    }
    for (int i = 0; i < n; i++) {   // right-aligned: the size comes from the descriptor, not from the value
        int s = have - n + i;
        t[2 + i] = s >= 0 ? v[s] : 0;
    }
    return 2 + n;
}

// A_GroupValue_Response with the value of object asap on group address ga. false: the link had no room for it.
static bool ko_answer(vdev_t *d, int asap, uint16_t ga)
{
    uint8_t b[2 + KNX_KO_VAL];
    int n = ko_tpdu(d, asap, 0x040, b);
    if (!n) {
        return true;
    }
    if (!tx(d, ga, true, PRIO_LOW, b, (uint8_t)n)) {
        return false;
    }
    d->grp_tx++;
    char gs[12];
    ga_str(ga, gs);
    ESP_LOGI(TAG, "dev %d: %s read -> KO %d answered", slot(d), gs, asap);
    return true;
}

static void group(vdev_t *d, const knx_frame_t *f)
{
    const uint8_t *g, *as, *t = f->tpdu;
    int ng = ga_count(d, &g), na = assoc_count(d, &as);
    uint16_t a = (uint16_t)((t[0] & 0x03) << 8 | t[1]);
    for (int i = 0; i < na; i++) {
        int ts = be16(as + 2 + 4 * i), asap = be16(as + 4 + 4 * i);
        if (ts < 1 || ts > ng || be16(g + 2 * ts) != f->dst || asap < 1 || asap > KO_MAX) {
            continue;
        }
        uint16_t desc = descriptor(d, asap);
        if (!(desc & 1 << 10)) {   // communication flag
            continue;
        }
        d->grp_rx++;
        s_hit = true;
        // A_GroupValue_Write with the write flag; A_GroupValue_Response with the update flag (03_05_01 Table 87,
        // bit 15) updates the object as a write does: a class that toggles needs the state another device reports
        if (((a & 0x3C0) == 0x080 && (desc & 1 << 12)) || ((a & 0x3C0) == 0x040 && (desc & 1 << 15))) {
            uint8_t six = t[1] & 0x3F;
            const uint8_t *v = f->len == 2 ? &six : t + 2;
            int n = f->len == 2 ? 1 : f->len - 2;
            if (n > KNX_KO_VAL) {   // an extended frame may carry more; no object of ours takes it
                ESP_LOGW(TAG, "dev %d: group write of %d octets to KO %d ignored, %d at most", slot(d), n, asap,
                         KNX_KO_VAL);
                continue;
            }
            char h[2 * KNX_KO_VAL + 1], gs[12];
            hex(v, n, h);
            ga_str(f->dst, gs);
            ESP_LOGI(TAG, "dev %d: %s -> KO %d = %s", slot(d), gs, asap, h);
            if (is_gw(d)) {
                gw_knx_write(asap, v, n);
            } else if (d->k >= 0) {
                zlink_knx_write(d->inst, asap, h);
            }
        } else if (a == 0x000 && (desc & 1 << 11) && d->ko[asap].have) {   // A_GroupValue_Read, read flag
            if (!ko_answer(d, asap, f->dst)) {   // the link had no room: timers() answers (it was dropped, and counted)
                d->resp_ko = (uint8_t)asap;
                d->resp_ga = f->dst;
            }
        }
    }
}

static bool ko_on(vdev_t *d, int asap, uint16_t ga)   // object asap is associated with group address ga
{
    const uint8_t *g, *a;
    int ng = ga_count(d, &g), na = assoc_count(d, &a);
    for (int i = 0; i < na; i++) {
        int ts = be16(a + 2 + 4 * i), as = be16(a + 4 + 4 * i);
        if (as == asap && ts >= 1 && ts <= ng && be16(g + 2 * ts) == ga) {
            return true;
        }
    }
    return false;
}

// The link gave up a group telegram of ours: no L_Data.con, an error flag of the chip, a reset in between. Nobody
// repeats it otherwise (the transport layer does that for management, a client asks again). A status value that is
// still the object's goes out again, at most LOST_MAX times, and so does an answer to a read. Values the class sent as
// commands are not repeated: one may have reached the bus after all, and twice is not once.
static void relost(const knx_frame_t *f)
{
    if (!f->group || !f->dst || f->len < 2) {
        return;
    }
    uint16_t a = (uint16_t)((f->tpdu[0] & 0x03) << 8 | f->tpdu[1]);
    bool wr = (a & 0x3C0) == 0x080, rsp = (a & 0x3C0) == 0x040;
    for (int i = 0; i < NDEV && (wr || rsp); i++) {
        vdev_t *d = &s_d[i];
        if (!d->used || d->ia != f->src) {
            continue;
        }
        for (int ko = 1; ko <= KO_MAX; ko++) {
            uint8_t b[2 + KNX_KO_VAL];
            if (!d->ko[ko].have || !ko_on(d, ko, f->dst) || (wr && !d->ko[ko].status)) {
                continue;
            }
            int n = ko_tpdu(d, ko, wr ? 0x080 : 0x040, b);
            if (n != f->len || memcmp(b, f->tpdu, (size_t)n) != 0) {
                continue;   // not this object, or another value went out since: that one counts
            }
            char gs[12];
            ga_str(f->dst, gs);
            if (d->ko[ko].lost >= LOST_MAX) {
                ESP_LOGW(TAG, "dev %d: KO %d -> %s given up by the link once more: not sent again", slot(d), ko, gs);
                continue;
            }
            d->ko[ko].lost++;
            if (wr) {
                d->ko[ko].unsent = 1;
            } else {
                d->resp_ko = (uint8_t)ko;
                d->resp_ga = f->dst;
            }
            ESP_LOGW(TAG, "dev %d: KO %d -> %s given up by the link: sent again", slot(d), ko, gs);
        }
    }
}

static vdev_t *by_inst(int inst)
{
    for (int i = 0; i < NDEV; i++) {
        if (s_d[i].used && s_d[i].inst == inst) {
            return &s_d[i];
        }
    }
    return NULL;
}

// a value the class hands out: status objects go on the bus when they change, ko_send always
// The value of a group object onto the bus. false: the link had no room, the caller keeps it for later.
static bool ko_put(vdev_t *d, int ko)
{
    uint16_t desc = descriptor(d, ko);
    int ga = ga_for_asap(d, ko);
    uint8_t b[2 + KNX_KO_VAL];
    int len = ko_tpdu(d, ko, 0x080, b);
    if (ga < 0 || !(desc & 1 << 14) || !(desc & 1 << 10) || !len) {   // no address, no transmit or communication flag
        return true;
    }
    if (!tx(d, (uint16_t)ga, true, PRIO_LOW, b, (uint8_t)len)) {
        return false;
    }
    d->grp_tx++;
    char gs[12], h[2 * KNX_KO_VAL + 1];
    ga_str((uint16_t)ga, gs);
    hex(d->ko[ko].v, d->ko[ko].len, h);
    ESP_LOGI(TAG, "dev %d: KO %d = %s -> %s", slot(d), ko, h, gs);
    return true;
}

// A status object of a virtual device the bus has not heard since the start or the device's restart: its value goes
// out with the device's other ones once they are in (tell_held)
static void hold(vdev_t *d, int ko)
{
    d->ko[ko].due = 1;
    if (!d->held_at) {
        d->held_at = esp_timer_get_time() + HELD_SETTLE_US;
    }
}

static void ko_out(int inst, int ko, const uint8_t *v, int n, bool initial, bool status)
{
    vdev_t *d = by_inst(inst);
    if (!d || ko < 1 || ko > KO_MAX || n < 1 || n > KNX_KO_VAL) {
        return;
    }
    bool changed = !d->ko[ko].have || d->ko[ko].len != n || memcmp(d->ko[ko].v, v, (size_t)n) != 0;
    if (changed) {
        d->ko[ko].lost = 0;
    }
    d->ko[ko].status = status;
    d->ko[ko].have = true;
    d->ko[ko].len = (uint8_t)n;
    memcpy(d->ko[ko].v, v, (size_t)n);
    if (initial) {
        // The value as linking found it is kept for a read. Its first one since the start or the device's restart goes
        // on the bus as well, a little later (found in a review); the gateway's own objects tell themselves (gw.c).
        if (status && !d->ko[ko].told && !is_gw(d)) {
            hold(d, ko);
        }
        return;
    }
    if (status && !changed && !d->ko[ko].unsent) {
        return;   // a held value that is due stays due: the bus has not heard it
    }
    d->ko[ko].unsent = !ko_put(d, ko);
    if (status) {
        d->ko[ko].told = 1;
        d->ko[ko].due = 0;
    }
}

// The status values a device holds that the bus has not heard since the start or the device's restart, together
static void tell_held(vdev_t *d)
{
    int n = 0;
    for (int ko = 1; ko <= KO_MAX; ko++) {
        if (!d->ko[ko].due) {
            continue;
        }
        d->ko[ko].due = 0;
        d->ko[ko].told = 1;
        if (d->ko[ko].have && d->ko[ko].status) {
            d->ko[ko].unsent = !ko_put(d, ko);
            n++;
        }
    }
    d->held_at = 0;
    if (n) {   // ko_put() sends nothing for an object without a group address or transmit flag
        ESP_LOGI(TAG, "dev %d: %d status values it holds told, the first since the start or its restart", slot(d), n);
    }
}

static void dispatch(const knx_frame_t *f)
{
    uint8_t tp = f->tpdu[0];
    if (f->group) {
        if ((tp & 0xFC) != 0x00 || f->len < 2) {   // T_Data_Group / T_Data_Broadcast
            return;
        }
        if (f->dst == 0 && !f->own && ((tp & 0x03) << 8 | f->tpdu[1]) == 0x100) {
            pair_knx_poll(f->src);   // A_IndividualAddress_Read: ETS waits for a device in programming mode
        }
        for (int i = 0; i < NDEV; i++) {
            vdev_t *d = &s_d[i];
            if (!d->used || (f->own && d->ia == f->src)) {   // a device does not hear itself
                continue;
            }
            if (f->dst == 0) {
                if (!f->own) {
                    broadcast(d, f);
                }
            } else {
                group(d, f);
            }
        }
        return;
    }
    if (f->own) {
        return;
    }
    vdev_t *d = NULL;
    for (int i = 0; i < NDEV && !d; i++) {
        // The factory address belongs to the device in programming mode. Without one nobody takes the frame, just
        // as nobody acknowledges it (publish()).
        if (s_d[i].used && s_d[i].ia == f->dst && (f->dst != IA_UNPROG || s_d[i].prog)) {
            d = &s_d[i];
        }
    }
    // T_Connect: ETS asks an address before it programs it, free or taken. Not the gateway's own: that one is
    // programmed with its button, like any device, and must never open the Zigbee network.
    if (tp == 0x80 && !(d && is_gw(d))) {
        pair_knx_probe(f->src, f->dst, d ? slot(d) : -1);
    }
    if (d) {
        s_hit = true;
        d->rx++;
        individual(d, f);
    }
}

static void timers(int64_t now)
{
    bool quiet = now - s_last_rx >= SAVE_QUIET_US, conn = false;
    for (int i = 0; i < NDEV; i++) {
        vdev_t *d = &s_d[i];
        conn_t *c = &d->c;
        if (!d->used) {
            continue;
        }
        if (d->prog && d->prog_until && now > d->prog_until) {
            set_prog(d, false);
            publish();
            ESP_LOGW(TAG, "dev %d: programming mode ended by itself after %d min", slot(d), (int)(PROG_US / 60000000));
            if (!is_gw(d)) {
                pair_knx_prog_ended(slot(d));   // a series that holds this device in programming mode is over
            }
        }
        if (c->on) {
            conn = true;
            if (c->qc && c->sent && now - c->sent > ACK_US) {
                if (++c->retries > 3) {
                    conn_drop(d, "no T_ACK");
                } else {
                    kick(d);
                }
            } else if (now - c->last > IDLE_US) {
                conn_drop(d, "idle");
            }
        }
        if (d->resp_ko && (!d->ko[d->resp_ko].have || ko_answer(d, d->resp_ko, d->resp_ga))) {   // an answer first
            d->resp_ko = 0;
        }
        for (int ko = 1; ko <= KO_MAX; ko++) {   // values the link had no room for
            if (d->ko[ko].unsent && (d->ko[ko].unsent = !ko_put(d, ko))) {
                break;
            }
        }
        // Content kept for an allocation that never came, or of an object in Error (nothing reads it until the Unload
        // that has to come first; it held up to 16 KiB of the budget till then). Never while a connection is open: a
        // download may still be running.
        for (int o = 1; o <= 5 && !d->c.on; o++) {
            lobj_t *l = &d->lo[o];
            if ((l->seg || l->ts1) && (!l->size || l->state == LS_ERROR) && l->keep_until && now > l->keep_until) {
                seg_drop(l);
                l->size = 0;
                l->keep_until = 0;
            }
        }
        if (d->retry_at && now > d->retry_at && !d->c.on) {   // its template found no memory: once more
            d->retry_at = 0;
            apply(d);
        }
        if (d->reparam && d->k >= 2 && now > d->reread_at) {   // the device has not told its model yet: ask again
            // 30 s, then twice as long each time up to 32 min: a sleepy device answers only when it is awake
            d->reread_at = now + (30000000LL << (d->reread_n < 6 ? d->reread_n : 6));
            d->reread_n++;
            if (!zlink_is_off(d->inst)) {   // not while it does not answer: its availability check asks anyway
                zlink_reread(d->inst);
            }
        }
        if (d->k >= 0 && zlink_params_due(d->inst)) {   // it answers again: what the class writes may have missed it
            send_params(d);
        }
    }
    // held status values, one device per HELD_GAP_US: 32 devices after a start take 10 s
    for (int j = 0; j < VDEV_MAX && now - s_held_t >= HELD_GAP_US; j++) {
        vdev_t *d = &s_d[(s_held_rr + j) % VDEV_MAX];
        if (d->used && d->held_at && now >= d->held_at) {
            tell_held(d);
            s_held_t = now;
            s_held_rr = (slot(d) + 1) % VDEV_MAX;
        }
    }
    // A flash write holds up the receive path, so it waits for a quiet moment of our own traffic. The wait has an
    // end: a busy line or a connection that stays open must not leave a download in RAM only.
    for (int i = 0; i < NDEV; i++) {
        vdev_t *d = &s_d[i];
        if (!d->used || !d->dirty || now < d->save_after) {
            continue;
        }
        int64_t age = now - d->dirty_since;
        if ((quiet && !conn) || (age > SAVE_LATE_US && !d->c.on) || age > SAVE_HARD_US) {
            save(d);
            break;   // one write per round
        }
    }
}

static void queued(QueueHandle_t q, volatile uint8_t *peak)   // after a send into q: high-water mark, wake the task
{
    UBaseType_t n = uxQueueMessagesWaiting(q);
    if (n > *peak) {
        *peak = (uint8_t)n;
    }
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

static void sink(const knx_frame_t *f)   // in the link task: never block
{
    ev_t e = {.type = EV_FRAME};
    e.f = *f;
    if (xQueueSend(s_q, &e, 0) != pdTRUE) {
        s_ev_lost++;
        return;
    }
    queued(s_q, &s_q_peak);
}

static void lost(const knx_frame_t *f)   // in the link task: a frame of ours it gave up, never block
{
    ev_t e = {.type = EV_LOST};
    e.f = *f;
    if (xQueueSend(s_q, &e, 0) != pdTRUE) {
        s_ev_lost++;
        return;
    }
    queued(s_q, &s_q_peak);
}

// from zlink.c, in the Berry task while a hook runs: never block, the device server may be waiting for that hook
void vdev_ko_out(int inst, int ko, const uint8_t *v, int n, bool initial, bool status)
{
    if (!s_kq || n < 1 || n > KNX_KO_VAL) {
        return;
    }
    kv_t k = {.inst = (int16_t)inst, .ko = (uint8_t)ko, .len = (uint8_t)n, .initial = initial, .status = status};
    memcpy(k.v, v, (size_t)n);
    if (xQueueSend(s_kq, &k, 0) != pdTRUE) {
        s_kv_lost++;
        return;
    }
    queued(s_kq, &s_kq_peak);
}

// What waits in both queues, a frame and a value at a time, each under the lock: neither kind waits for all of the
// other, and the lock is free in between for the console and the gateway's task.
static void drain(void)
{
    for (;;) {
        ev_t e;
        kv_t k;
        bool f = xQueueReceive(s_q, &e, 0) == pdTRUE, v = xQueueReceive(s_kq, &k, 0) == pdTRUE;
        if (!f && !v) {
            return;
        }
        wdt_feed();   // one step: a busy line keeps this loop going, each frame and value ends by itself
        int64_t now = esp_timer_get_time();
        xSemaphoreTake(s_mx, portMAX_DELAY);
        if (f && e.type == EV_LOST) {
            relost(&e.f);
        } else if (f) {
            s_hit = false;
            dispatch(&e.f);
            if (s_hit) {   // only traffic of our own devices keeps the flash waiting, not everything on the line
                s_last_rx = now;
            }
        }
        if (v) {
            ko_out(k.inst, k.ko, k.v, k.len, k.initial, k.status);
        }
        xSemaphoreGive(s_mx);
    }
}

// From the gateway's task (gw.c), which holds no other lock: straight into the gateway's object, nothing is lost on the
// way. Through the queue a value could be dropped while the task's comparison value already said it was put.
void vdev_gw_put(int ko, const uint8_t *v, int n, int how)
{
    if (!s_mx) {
        return;
    }
    xSemaphoreTake(s_mx, portMAX_DELAY);
    ko_out(VDEV_GW_INST, ko, v, n, how == VDEV_KEEP, how != VDEV_SEND);
    xSemaphoreGive(s_mx);
}

static void vdev_task(void *arg)
{
    (void)arg;
    int64_t tick = 0;
    wdt_watch();
    for (;;) {
        wdt_feed();
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
        drain();
        int64_t now = esp_timer_get_time();
        if (now - tick >= 100000) {
            tick = now;
            xSemaphoreTake(s_mx, portMAX_DELAY);
            timers(now);
            xSemaphoreGive(s_mx);
        }
    }
}

// ---------------------------------------------------------------- start and console
// Once, at the first start with 32 slots: the gateway's record "v8" (8 slots then) becomes "vgw", and "layout" says
// it happened. Without "layout" a "v8" can only be the gateway's: the firmware before had no virtual device 8. If
// "vgw" is there already (a restart between the steps, or a failed move after which the gateway was written anew),
// it is the newer one and "v8" just goes. false: not done, "v8" must not be read as virtual device 8 meanwhile.
static bool gw_key_move(void)
{
    static const uint8_t LAYOUT = 32;
    size_t n = 0, m = 0;
    if (nvs_get_blob(s_nvs, "layout", NULL, &n) == ESP_OK) {
        return true;
    }
    if (nvs_get_blob(s_nvs, "v8", NULL, &n) == ESP_OK) {
        esp_err_t e = ESP_OK;
        if (nvs_get_blob(s_nvs, "vgw", NULL, &m) != ESP_OK) {
            uint8_t *b = malloc(n ? n : 1);
            e = b ? nvs_get_blob(s_nvs, "v8", b, &n) : ESP_FAIL;
            if (e == ESP_OK) {
                e = nvs_set_blob(s_nvs, "vgw", b, n);
            }
            if (e == ESP_OK) {
                e = nvs_commit(s_nvs);
            }
            free(b);
        }
        if (e == ESP_OK) {
            e = nvs_erase_key(s_nvs, "v8");
        }
        if (e == ESP_OK) {
            e = nvs_commit(s_nvs);
        }
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "moving the gateway's record v8 to vgw failed (%s); again at the next start",
                     esp_err_to_name(e));
            return false;
        }
        ESP_LOGI(TAG, "the gateway's record moved from v8 to vgw (%u B)", (unsigned)n);
    }
    if (nvs_set_blob(s_nvs, "layout", &LAYOUT, 1) != ESP_OK || nvs_commit(s_nvs) != ESP_OK) {
        return false;
    }
    return true;
}

// At the start, after every record was read: a template part no record names any more goes (a restart came between
// blob and record, or firmware before this one left it). From then on a part goes as soon as its last record does.
static void ts_sweep(void)
{
    s_swept = true;
    static char gone[16][16];   // more go at the next start
    int n = 0;
    nvs_iterator_t it = NULL;
    esp_err_t e = nvs_entry_find(NVS_PART, "vdev", NVS_TYPE_BLOB, &it);
    while (e == ESP_OK && n < 16) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        bool named = info.key[0] != 't';
        for (int i = 0; i < NDEV && !named; i++) {
            char k[16];
            ts_key(s_tref[i].crc, s_tref[i].len, k);
            named = s_tref[i].len && strcmp(k, info.key) == 0;
        }
        if (!named) {
            snprintf(gone[n++], 16, "%s", info.key);
        }
        e = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    for (int i = 0; i < n; i++) {
        ESP_LOGI(TAG, "template part %s: no record names it any more, deleted", gone[i]);
        nvs_erase_key(s_nvs, gone[i]);
    }
    if (n) {
        nvs_commit(s_nvs);
    }
}

void vdev_start(void)
{
    s_swept = false;   // until every record is read: one of them may name a part that looks unnamed
    s_mx = xSemaphoreCreateMutex();
    s_q = xQueueCreate(FRAME_Q, sizeof(ev_t));
    s_kq = xQueueCreate(VALUE_Q, sizeof(kv_t));
    // The task runs before the devices come back: each one's class sends its first values while the next device is
    // brought up, and they are taken whenever the lock is free. Before, all of them waited for the whole start, and
    // with 32 lamps of 2 to 4 values each the queue was full long before the task ran.
    xTaskCreate(vdev_task, "vdev", 5120, NULL, 6, &s_task);
    xSemaphoreTake(s_mx, portMAX_DELAY);
    esp_err_t e = nvs_flash_init_partition(NVS_PART);
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // NVS cannot use the partition as it is: it is erased, and with it every device ETS loaded
        ESP_LOGE(TAG, "partition %s: %s; ERASED, the records of every virtual device and the gateway are lost",
                 NVS_PART, esp_err_to_name(e));
        gw_note(GW_NOTE_STORE, 0xFFFF, 0, "the records' partition had to be erased at the start: every device is lost");
        nvs_flash_erase_partition(NVS_PART);
        e = nvs_flash_init_partition(NVS_PART);
    }
    if (e == ESP_OK) {
        e = nvs_open_from_partition(NVS_PART, "vdev", NVS_READWRITE, &s_nvs);
    }
    s_nvs_ok = e == ESP_OK;
    if (!s_nvs_ok) {   // the gateway's fault stays on (vdev_store_failing())
        ESP_LOGE(TAG, "partition %s: %s; devices are not kept over a restart", NVS_PART, esp_err_to_name(e));
        gw_note(GW_NOTE_STORE, 0xFFFF, 0, "the records' partition cannot be used: devices are not kept over a restart");
    }
    // Every record first: which template parts they name decides what ts_sweep() deletes
    int n = 0;
    s_v8_blocked = s_nvs_ok && !gw_key_move();   // until the move is done, slot 8 is neither read nor handed out
    for (int i = 0; i < VDEV_MAX && s_nvs_ok; i++) {
        if (!(i == 8 && s_v8_blocked) && restore(i)) {
            vdev_t *d = &s_d[i];
            char a[12];
            ia_str(d->ia, a);
            ESP_LOGI(TAG, "dev %d: restored, %s, load states %u %u %u %u %u, parameter segment %u B", i, a,
                     d->lo[1].state, d->lo[2].state, d->lo[3].state, d->lo[4].state, d->lo[5].state,
                     (unsigned)d->lo[4].size);
            n++;
        }
    }
    vdev_t *g = &s_d[GW];   // the gateway's own device is always there; its parameters go to gw.c
    bool gw_back = s_nvs_ok && restore(GW);
    if (!gw_back) {
        init_dev(g);
        ESP_LOGI(TAG, "gateway: not loaded yet, 15.15.255");
    }
    if (s_nvs_ok) {
        ts_sweep();
    }
    s_loaded = true;
    xSemaphoreGive(s_mx);
    // Then one device after the other, the lock free in between for the task
    for (int i = 0; i < VDEV_MAX; i++) {
        xSemaphoreTake(s_mx, portMAX_DELAY);
        vdev_t *d = &s_d[i];
        if (d->used && d->zaddr != ZADDR_NONE) {
            apply(d);   // the template or the class of the last download
            // A device set up by hand that ETS has not loaded yet runs the built-in class it was given. An application
            // that is loaded and does not come up runs nothing, as at run time: its load state reads Error (earlier
            // firmware fell back to the built-in class of the application number, with that class's defaults and
            // the tables of whatever version was loaded).
            int k = d->lo[4].state == LS_UNLOADED ? d->k_hint : -1;
            if (d->k < 0 && k >= 0 && zlink_set(d->inst, k, d->zaddr, d->zep) == 0) {
                d->k = (int8_t)k;
                if (d->gone) {   // as in apply(): a device that left stays off until it is back
                    zlink_gone(d->inst, true);
                }
            }
        }
        xSemaphoreGive(s_mx);
    }
    xSemaphoreTake(s_mx, portMAX_DELAY);
    for (int i = 0; i < NDEV; i++) {   // records written before the check above may share an address: told, kept
        vdev_t *o = s_d[i].used ? ia_owner(s_d[i].ia, &s_d[i]) : NULL;
        if (o && o > &s_d[i]) {
            char a[12], why[96];
            ia_str(s_d[i].ia, a);
            snprintf(why, sizeof why, "devices %d and %d both have address %s: frames reach only the first", i, slot(o),
                     a);
            ESP_LOGE(TAG, "%s", why);
            gw_note(GW_NOTE_DUP, s_d[i].ia, 0, why);
        }
    }
    if (gw_back) {
        char a[12];
        ia_str(g->ia, a);
        ESP_LOGI(TAG, "gateway: restored, %s, load states %u %u %u %u %u, parameter segment %u B", a, g->lo[1].state,
                 g->lo[2].state, g->lo[3].state, g->lo[4].state, g->lo[5].state, (unsigned)g->lo[4].size);
        apply(g);
    }
    publish();
    xSemaphoreGive(s_mx);
    knx_link_sink(sink);
    knx_link_lost(lost);
    ESP_LOGI(TAG, "device server up, %d virtual devices and the gateway", n);
}

// k: built-in class to run until a download says what the device is, -1: none (a device that has just joined gets
// nothing written before ETS has loaded it). ieee: the Zigbee device's IEEE address if known.
int vdev_add(uint16_t zaddr, uint8_t ep, int k, const uint8_t *ieee)
{
    int r = -1;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    for (int i = 0; i < VDEV_MAX && r < 0; i++) {
        if (!s_d[i].used && !(i == 8 && s_v8_blocked)) {
            r = i;
        }
    }
    if (r >= 0) {
        vdev_t *d = &s_d[r];
        init_dev(d);
        d->zaddr = zaddr;
        d->zep = ep;
        d->k_hint = k == 0 || k == 1 ? (int8_t)k : -1;
        if (ieee) {
            memcpy(d->ieee, ieee, 8);
            d->have_ieee = true;
            serial_of(d);
        }
        mark(d);
        publish();
    }
    xSemaphoreGive(s_mx);
    if (r >= 0 && zaddr != ZADDR_NONE && (k == 0 || k == 1)) {   // outside the lock: linking runs Berry hooks
        int e = zlink_set(100 + r, k, zaddr, ep);
        xSemaphoreTake(s_mx, portMAX_DELAY);
        if (e == 0) {
            s_d[r].k = (int8_t)k;
        } else {
            ESP_LOGE(TAG, "dev %d: link to 0x%04x failed (%d)", r, zaddr, e);
        }
        xSemaphoreGive(s_mx);
    }
    return r;
}

bool vdev_del(int i)
{
    if (i < 0 || i >= VDEV_MAX) {
        return false;
    }
    xSemaphoreTake(s_mx, portMAX_DELAY);
    bool was = s_d[i].used;
    if (was) {
        for (int o = 1; o <= 5; o++) {
            seg_drop(&s_d[i].lo[o]);
        }
        memset(&s_d[i], 0, sizeof s_d[i]);
        if (s_nvs_ok) {
            char key[12];
            rec_key(i, key);
            nvs_erase_key(s_nvs, key);
            nvs_commit(s_nvs);
            tref_set(i, 0, 0);   // its template part goes with it unless another record names it
        }
        publish();
        zlink_del(100 + i);          // the VM forgets the instance, the Zigbee device is free for another one
        bvm_keep(slots_in_use());    // a template VM nobody uses any more goes; under the lock, as in apply()
    }
    xSemaphoreGive(s_mx);
    return was;
}

int vdev_free_slots(void)
{
    int n = 0;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    for (int i = 0; i < VDEV_MAX; i++) {
        n += !s_d[i].used && !(i == 8 && s_v8_blocked);
    }
    xSemaphoreGive(s_mx);
    return n;
}

int vdev_by_ieee(const uint8_t ieee[8])   // the virtual device that serves this Zigbee device, -1: none
{
    int r = -1;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    for (int i = 0; i < VDEV_MAX && r < 0; i++) {
        if (s_d[i].used && s_d[i].have_ieee && memcmp(s_d[i].ieee, ieee, 8) == 0) {
            r = i;
        }
    }
    xSemaphoreGive(s_mx);
    return r;
}

// A Zigbee device showed up with its short and its IEEE address. A device we serve is known by the IEEE address
// from then on; one that came back with another short address is followed there. true: it is one of ours.
bool vdev_seen(uint16_t zaddr, const uint8_t ieee[8])
{
    int moved = -1, k = -1, inst = 0, back = 0;   // moved: a class given by hand follows its device (no download)
    uint8_t ep = 1;
    bool ours = false;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    for (int i = 0; i < VDEV_MAX; i++) {
        vdev_t *d = &s_d[i];
        if (!d->used || d->zaddr == ZADDR_NONE) {
            continue;
        }
        if (d->have_ieee && memcmp(d->ieee, ieee, 8) == 0) {
            ours = true;
            if (d->gone) {   // it had left the network and is back: nothing to replace any more
                d->gone = false;
                mark(d);
                ESP_LOGI(TAG, "dev %d: its Zigbee device is back in the network", i);
            }
            if (d->zaddr != zaddr) {
                ESP_LOGW(TAG, "dev %d: its Zigbee device came back as 0x%04x (was 0x%04x)", i, zaddr, d->zaddr);
                d->zaddr = zaddr;
                mark(d);
                if (d->lo[4].state == LS_LOADED && d->lo[4].size) {
                    // As after a start: apply() links the class at the new address and hands it the parameters of the
                    // download again. zlink_set() alone gave a built-in class its default segment.
                    apply(d);
                } else {   // a class given by hand, nothing downloaded: its defaults are all it has
                    moved = i;
                    k = d->k;
                    inst = d->inst;
                    ep = d->zep;
                }
            } else if (d->k >= 0) {   // same address: power back or a rejoin, what it shows now may have changed
                back = d->inst;
            }
        } else if (!d->have_ieee && d->zaddr == zaddr) {   // set up before the address was kept: learn it
            memcpy(d->ieee, ieee, 8);
            d->have_ieee = true;
            serial_of(d);
            mark(d);
            ours = true;
        }
    }
    xSemaphoreGive(s_mx);
    if (moved >= 0 && k >= 0) {
        zlink_set(inst, k, zaddr, ep);   // outside the lock, as in vdev_add()
    }
    if (back) {
        zlink_announced(back);
    }
    return ours;
}

int vdev_of_zigbee(uint16_t zaddr)   // the virtual device behind this Zigbee short address, -1: none
{
    int r = -1;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    for (int i = 0; i < VDEV_MAX && r < 0; i++) {
        if (s_d[i].used && s_d[i].zaddr == zaddr && zaddr != ZADDR_NONE) {
            r = i;
        }
    }
    xSemaphoreGive(s_mx);
    return r;
}

bool vdev_unbound(int i)   // a Zigbee device behind it and no individual address yet
{
    if (i < 0 || i >= VDEV_MAX) {
        return false;
    }
    xSemaphoreTake(s_mx, portMAX_DELAY);
    bool u = s_d[i].used && s_d[i].ia == IA_UNPROG && s_d[i].zaddr != ZADDR_NONE;
    xSemaphoreGive(s_mx);
    return u;
}

bool vdev_unprogrammed(int i)   // in use and without an individual address, with or without a Zigbee device
{
    if (i < 0 || i >= VDEV_MAX) {
        return false;
    }
    xSemaphoreTake(s_mx, portMAX_DELAY);
    bool u = s_d[i].used && s_d[i].ia == IA_UNPROG;
    xSemaphoreGive(s_mx);
    return u;
}

int vdev_rx_on(int i)   // its Zigbee device keeps its receiver on: 1, it sleeps: 0, not known (or no such device): -1
{
    if (i < 0 || i >= VDEV_MAX) {
        return -1;
    }
    xSemaphoreTake(s_mx, portMAX_DELAY);
    int r = s_d[i].used && s_d[i].cap_known ? s_d[i].rx_on : -1;
    xSemaphoreGive(s_mx);
    return r;
}

bool vdev_ieee(int i, uint8_t ieee[8])   // false: no such device, or its IEEE address is not known
{
    if (i < 0 || i >= VDEV_MAX) {
        return false;
    }
    xSemaphoreTake(s_mx, portMAX_DELAY);
    bool h = s_d[i].used && s_d[i].have_ieee;
    if (h) {
        memcpy(ieee, s_d[i].ieee, 8);
    }
    xSemaphoreGive(s_mx);
    return h;
}

// Its Zigbee device left the network (leave indication). The virtual device keeps address, tables, parameters and
// template; the next ETS question for its address puts a new Zigbee device in its place (pair.c).
void vdev_gone(int i)
{
    if (i < 0 || i >= VDEV_MAX) {
        return;
    }
    xSemaphoreTake(s_mx, portMAX_DELAY);
    vdev_t *d = &s_d[i];
    if (d->used && d->zaddr != ZADDR_NONE && !d->gone) {
        char a[12];
        ia_str(d->ia, a);
        d->gone = true;
        mark(d);
        zlink_gone(d->inst, true);   // off at once, and not asked under its old address
        ESP_LOGW(TAG, "dev %d (%s): its Zigbee device 0x%04x left the network; the next ETS question for %s takes a "
                 "new one in its place", i, a, d->zaddr, a);
    }
    xSemaphoreGive(s_mx);
}

bool vdev_is_gone(int i)
{
    if (i < 0 || i >= VDEV_MAX) {
        return false;
    }
    xSemaphoreTake(s_mx, portMAX_DELAY);
    bool g = s_d[i].used && s_d[i].gone;
    xSemaphoreGive(s_mx);
    return g;
}

// A new Zigbee device takes the place of the one that left: same address, tables, parameters and template. The class
// instance moves to the new device and gets the parameters again, so the device is set up like the one before.
bool vdev_replace(int i, uint16_t zaddr, const uint8_t ieee[8])
{
    if (i < 0 || i >= VDEV_MAX) {
        return false;
    }
    xSemaphoreTake(s_mx, portMAX_DELAY);
    vdev_t *d = &s_d[i];
    bool ok = d->used;
    if (ok) {
        char a[12];
        ia_str(d->ia, a);
        ESP_LOGI(TAG, "dev %d (%s): Zigbee device 0x%04x takes the place of 0x%04x", i, a, zaddr, d->zaddr);
        // The class forgets the device that left: on 2026-11-13 the instance still knew the plug's model when its
        // parameters came, reported the lamp as listed and wrote the plug's child lock to it. Without the model the
        // parameters come once more when the new device has told it (reparam).
        zlink_del(d->inst);
        d->zaddr = zaddr;
        memcpy(d->ieee, ieee, 8);
        d->have_ieee = true;
        serial_of(d);   // the new Zigbee device's own number: ETS sees another serial number at this address
        d->gone = false;
        d->model_ok = 0;
        mark(d);
        apply(d);   // links the class to the new device and hands it the parameters
    }
    xSemaphoreGive(s_mx);
    return ok;
}

void vdev_test_refuse(uint8_t err)   // console tplfail: the next check at LoadCompleted fails with err, 0 cancels
{
    s_test_refuse = err;
}

// From the pair task: the device with this IEEE address announced itself. Bit 3 of the MAC capability flags is
// "receiver on when idle" (IEEE 802.15.4 capability information, as the device announce carries it).
static void set_capability(const uint8_t *ieee, uint16_t zaddr, uint8_t capability)
{
    bool rx_on = capability & 0x08;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    for (int i = 0; i < VDEV_MAX; i++) {
        vdev_t *d = &s_d[i];
        if (!d->used || (ieee ? !d->have_ieee || memcmp(d->ieee, ieee, 8) != 0 : d->zaddr != zaddr)) {
            continue;
        }
        if (!d->cap_known || d->rx_on != rx_on) {
            d->cap_known = true;
            d->rx_on = rx_on;
            mark(d);
            ESP_LOGI(TAG, "dev %d: its Zigbee device keeps its receiver %s (capability 0x%02x)", i,
                     rx_on ? "on" : "off when idle: a sleepy end device", capability);
        }
        zlink_sleepy(d->inst, !rx_on);
    }
    xSemaphoreGive(s_mx);
}

void vdev_set_capability(const uint8_t ieee[8], uint8_t capability)
{
    set_capability(ieee, ZADDR_NONE, capability);
}

void vdev_set_capability_of(uint16_t zaddr, uint8_t capability)   // from the interview's node descriptor
{
    if (zaddr != ZADDR_NONE) {
        set_capability(NULL, zaddr, capability);
    }
}

uint8_t vdev_endpoint_of(uint16_t zaddr)
{
    uint8_t ep = 0;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    for (int i = 0; i < VDEV_MAX && !ep; i++) {
        if (s_d[i].used && s_d[i].zaddr == zaddr && zaddr != ZADDR_NONE) {
            ep = s_d[i].zep;
        }
    }
    xSemaphoreGive(s_mx);
    return ep;
}

// From the link task (no lock of zlink.c held): the interview found the function of the Zigbee device at zaddr on
// endpoint ep. Kept in the record; a linked class moves there (zlink_set with the new endpoint reads the device anew).
void vdev_set_endpoint(uint16_t zaddr, uint8_t ep)
{
    if (zaddr == ZADDR_NONE || !ep) {
        return;
    }
    xSemaphoreTake(s_mx, portMAX_DELAY);
    for (int i = 0; i < VDEV_MAX; i++) {
        vdev_t *d = &s_d[i];
        if (!d->used || d->zaddr != zaddr || d->zep == ep) {
            continue;
        }
        char a[12];
        ia_str(d->ia, a);
        ESP_LOGI(TAG, "dev %d (%s): Zigbee device 0x%04x has its function on endpoint %u, not %u", i, a, zaddr, ep,
                 d->zep);
        d->zep = ep;
        mark(d);
        // As after a start: apply() links the class on the new endpoint and hands it the parameters of the download
        // again (zlink_set() alone gave a built-in class its default segment)
        apply(d);
    }
    xSemaphoreGive(s_mx);
}

void vdev_avail_note(int inst, bool on)   // from the link task, which holds no lock of zlink.c then
{
    xSemaphoreTake(s_mx, portMAX_DELAY);
    vdev_t *d = by_inst(inst);
    uint16_t ia = d ? d->ia : 0xFFFF;
    xSemaphoreGive(s_mx);
    if (d) {
        gw_note(on ? GW_NOTE_ONLINE : GW_NOTE_OFFLINE, ia, 0,
                on ? "its Zigbee device answers again" : "its Zigbee device does not answer");
    }
}

int vdev_max_apdu(int n)   // PID 56 of every device, 15 .. KNX_TPDU_MAX - 1; 0 only reads it. No lock: one octet
{
    if (n >= MAX_APDU_DEF && n <= KNX_TPDU_MAX - 1) {
        s_max_apdu = (uint8_t)n;
    }
    return s_max_apdu;
}

bool vdev_any_prog(void)   // the gateway counts: in programming mode it is the one that answers ETS
{
    bool any = false;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    for (int i = 0; i < NDEV; i++) {
        any = any || (s_d[i].used && s_d[i].prog);
    }
    xSemaphoreGive(s_mx);
    return any;
}

bool vdev_gw_prog(void)   // without the lock: one flag, read by the button task every 20 ms
{
    return s_d[GW].prog;
}

bool vdev_vprog(void)   // a virtual device (not the gateway's own) is in programming mode; without the lock, as above
{
    return s_vprog;
}

bool vdev_store_failing(void)   // the gateway's fault: no storage, or a record waits after a failed write
{
    bool f = !s_nvs_ok;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    for (int i = 0; i < NDEV && !f; i++) {
        f = s_d[i].used && s_d[i].dirty && s_d[i].save_fails;
    }
    xSemaphoreGive(s_mx);
    return f;
}

int vdev_faulty(void)   // the gateway's diagnostics: applications in Error or not running as loaded, its own included
{
    int n = 0;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    for (int i = 0; i < NDEV; i++) {
        const lobj_t *o = &s_d[i].lo[4];
        n += s_d[i].used && (o->state == LS_ERROR || (s_d[i].fault && o->state == LS_LOADED));
    }
    xSemaphoreGive(s_mx);
    return n;
}

// NVS entries of the records' partition in use, percent of those that can be used; -1: no storage. total_entries
// counts the page NVS keeps free for itself (fillStats), so used against total never reached 100 % (87.5 % with the
// 8 pages of the TULX32's actor_db).
int vdev_store_pct(void)
{
    nvs_stats_t st;
    size_t usable;
    if (!s_nvs_ok || nvs_get_stats(NVS_PART, &st) != ESP_OK || !(usable = st.used_entries + st.available_entries)) {
        return -1;
    }
    return (int)((st.used_entries * 100 + usable / 2) / usable);
}

bool vdev_loaded(void)
{
    return s_loaded;
}

void vdev_counts(int *bound, int *waiting)
{
    int b = 0, w = 0;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    for (int i = 0; i < VDEV_MAX; i++) {
        const vdev_t *d = &s_d[i];
        if (d->used && d->zaddr != ZADDR_NONE) {
            b += d->ia != IA_UNPROG;
            w += d->ia == IA_UNPROG;
        }
    }
    xSemaphoreGive(s_mx);
    *bound = b;
    *waiting = w;
}

int vdev_first_unbound(void)   // a device with a Zigbee link and without an individual address, -1: none
{
    int r = -1;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    for (int i = 0; i < VDEV_MAX && r < 0; i++) {
        if (s_d[i].used && s_d[i].ia == IA_UNPROG && s_d[i].zaddr != ZADDR_NONE) {
            r = i;
        }
    }
    xSemaphoreGive(s_mx);
    return r;
}

bool vdev_has_zigbee(uint16_t zaddr)
{
    bool has = false;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    for (int i = 0; i < VDEV_MAX; i++) {
        has = has || (s_d[i].used && s_d[i].zaddr == zaddr);
    }
    xSemaphoreGive(s_mx);
    return has;
}

bool vdev_zigbee(int i, uint16_t *zaddr, uint8_t *ep)
{
    if (i < 0 || i >= VDEV_MAX) {
        return false;
    }
    xSemaphoreTake(s_mx, portMAX_DELAY);
    bool ok = s_d[i].used && s_d[i].zaddr != ZADDR_NONE;
    *zaddr = s_d[i].zaddr;
    *ep = s_d[i].zep;
    xSemaphoreGive(s_mx);
    return ok;
}

void vdev_model_event(int inst)   // from zlink.c: the device told its model
{
    xSemaphoreTake(s_mx, portMAX_DELAY);
    vdev_t *d = by_inst(inst);
    if (d && model_check(d) != -2 && d->reparam) {
        d->reparam = false;
        d->reread_n = 0;
        send_params(d);   // the class knows its model now
    }
    xSemaphoreGive(s_mx);
}

bool vdev_prog(int i, bool on)   // also the gateway (VDEV_GW)
{
    if (i < 0 || i >= NDEV) {
        return false;
    }
    xSemaphoreTake(s_mx, portMAX_DELAY);
    bool ok = s_d[i].used;
    if (ok) {
        set_prog(&s_d[i], on);
        publish();
    }
    xSemaphoreGive(s_mx);
    return ok;
}

bool vdev_set_ia(int i, uint16_t ia)   // also the gateway (VDEV_GW); false: no such device, or the address is taken
{
    if (i < 0 || i >= NDEV) {
        return false;
    }
    xSemaphoreTake(s_mx, portMAX_DELAY);
    bool ok = s_d[i].used && ia_free_for(&s_d[i], ia);
    if (ok) {
        s_d[i].ia = ia;
        mark(&s_d[i]);
        publish();
    }
    xSemaphoreGive(s_mx);
    if (ok && i == GW) {
        gw_ia(ia);
    }
    return ok;
}

// What the console prints is put together under the device server's lock and printed after it: on the TULX32 the
// console is UART0 at 115200 Bd, where 32 devices took over a second to print, and no frame was handled meanwhile.
#define PRINT_BUF   1536
#define PRINT_ASSOC 16   // association lines per hold of the lock
typedef struct {
    char *b;
    size_t n, len;
} pbuf_t;

static void pf(pbuf_t *p, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void pf(pbuf_t *p, const char *fmt, ...)
{
    if (p->len + 1 >= p->n) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(p->b + p->len, p->n - p->len, fmt, ap);
    va_end(ap);
    if (w > 0) {
        p->len += (size_t)w < p->n - p->len ? (size_t)w : p->n - p->len - 1;
    }
}

void vdev_dump(int i, int obj)
{
    if (i < 0 || i >= NDEV || obj < 1 || obj > 5) {
        return;
    }
    char head[160];
    pbuf_t p = {.b = head, .n = sizeof head};
    head[0] = 0;
    uint32_t size = 0;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    vdev_t *d = &s_d[i];
    lobj_t *o = &d->lo[obj];
    if (d->used) {
        size = o->size;
        pf(&p, "dev %d obj %d: state %u, ref 0x%04x, size %u, crc %04x", i, obj, o->state, (unsigned)o->ref,
           (unsigned)o->size, seg_crc(o));
        if (o->ts1) {
            pf(&p, ", octets from %u on kept once (%u devices)", (unsigned)o->at, (unsigned)s_ts[o->ts1 - 1].refs);
        }
        pf(&p, "\n");
    }
    xSemaphoreGive(s_mx);
    printf("%s", head);
    for (uint32_t a = 0; a < size; a += 32) {   // a row at a time: the segment may change in between (a download)
        uint8_t row[32];
        uint32_t m = 0;
        xSemaphoreTake(s_mx, portMAX_DELAY);
        if (d->used && a < o->size) {
            m = o->size - a < 32 ? o->size - a : 32;
            seg_read(o, a, row, m);
        }
        xSemaphoreGive(s_mx);
        if (!m) {
            break;
        }
        char line[8 + 2 * 32 + 2];
        int k = snprintf(line, sizeof line, "%04x ", (unsigned)a);
        for (uint32_t j = 0; j < m; j++) {
            k += snprintf(line + k, sizeof line - (size_t)k, "%02x", row[j]);
        }
        printf("%s\n", line);
    }
}

// one device's lines from association `from` on (0: with its head); the next association to print, -1: done
static int print_dev(pbuf_t *p, vdev_t *d, int from, int *count)
{
    static const char *const LS[] = {"unloaded", "loaded", "loading", "error"};
    static const char *const TP[] = {"", ", embedded template", ", EMBEDDED TEMPLATE REFUSED"};
    const uint8_t *g, *as;
    int ng = ga_count(d, &g), na = assoc_count(d, &as), i = slot(d);
    if (!from) {
        char a[12], sn[13], pv[11];
        ia_str(d->ia, a);
        hex(d->serial, 6, sn);
        hex(d->lo[4].pv, 5, pv);
        if (is_gw(d)) {
            pf(p, "gateway (vdev %d): %s%s, serial %s, application of the firmware%s%s%s\n", i, a,
               d->prog ? " PROGRAMMING MODE" : "", sn, d->c.on ? ", connected" : "", d->dirty ? ", not saved yet" : "",
               d->save_fails ? ", ITS RECORD DOES NOT GET INTO THE FLASH" : "");
        } else {
            (*count)++;
            pf(p, "vdev %d: %s%s, serial %s, instance %d in slot %d (%s%s%s) <-> Zigbee 0x%04x ep %u%s%s%s%s%s\n", i, a,
               d->prog ? " PROGRAMMING MODE" : "", sn, d->inst, d->k, bvm_slot_name(d->k), TP[d->tpl],
               d->model_ok == 2 ? ", MODEL NOT IN THE TEMPLATE'S LIST" : d->model_ok == 1 ? ", model listed" : "",
               d->zaddr, d->zep,
               d->zaddr == ZADDR_NONE ? "" : !d->cap_known ? " (receiver: not announced yet)"
                                        : d->rx_on      ? " (receiver on)"
                                                        : " (sleepy end device)",
               d->gone ? " (LEFT THE NETWORK, waits for a replacement)" : "", d->c.on ? ", connected" : "",
               d->dirty ? ", not saved yet" : "", d->save_fails ? ", ITS RECORD DOES NOT GET INTO THE FLASH" : "");
        }
        char app[40];
        if (d->lo[4].state == LS_ERROR) {
            snprintf(app, sizeof app, "ERROR %u", d->lo[4].err);
        } else if (d->fault && d->lo[4].state == LS_LOADED) {
            snprintf(app, sizeof app, "loaded, NOT RUNNING (error %u)", d->fault);
        } else {
            snprintf(app, sizeof app, "%s", LS[d->lo[4].state]);
        }
        pf(p, "        tables %s/%s/%s, application %s (%s, %u B), PEI %s; %d group addresses, %d associations\n",
           LS[d->lo[1].state], LS[d->lo[2].state], LS[d->lo[3].state], app, pv, (unsigned)d->lo[4].size,
           LS[d->lo[5].state], ng, na);
    }
    int j = from;
    for (; j < na && j < from + PRINT_ASSOC; j++) {
        int ts = be16(as + 2 + 4 * j), asap = be16(as + 4 + 4 * j);
        char gs[12] = "?";
        if (ts >= 1 && ts <= ng) {
            ga_str(be16(g + 2 * ts), gs);
        }
        pf(p, "        %s <-> KO %d (descriptor %04x)\n", gs, asap, descriptor(d, asap));
    }
    if (j < na) {
        return j;
    }
    pf(p, "        frames in %lu, out %lu; group in %lu, out %lu; unhandled services %lu\n", (unsigned long)d->rx,
       (unsigned long)d->tx, (unsigned long)d->grp_rx, (unsigned long)d->grp_tx, (unsigned long)d->unhandled);
    return -1;
}

void vdev_print(void)
{
    pbuf_t p = {.b = malloc(PRINT_BUF), .n = PRINT_BUF};
    if (!p.b) {
        printf("vdev: no memory to print\n");
        return;
    }
    int n = 0;
    for (int i = 0; i < NDEV; i++) {
        for (int from = 0; from >= 0;) {
            p.len = 0;
            p.b[0] = 0;
            xSemaphoreTake(s_mx, portMAX_DELAY);
            from = s_d[i].used ? print_dev(&p, &s_d[i], from, &n) : -1;
            xSemaphoreGive(s_mx);
            printf("%s", p.b);
        }
    }
    p.len = 0;
    p.b[0] = 0;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    pf(&p, "vdev: %d virtual devices; waiting at most %u of %d frames (%lu lost) and %u of %d class values (%lu lost); "
       "%lu saves (%lu failed), segments %u of %u B%s; PID 56 (max APDU) %u%s\n", n, s_q_peak, FRAME_Q,
       (unsigned long)s_ev_lost, s_kq_peak, VALUE_Q, (unsigned long)s_kv_lost, (unsigned long)s_saves,
       (unsigned long)s_save_errs, (unsigned)seg_total(), (unsigned)SEG_BUDGET, s_nvs_ok ? "" : ", NO STORAGE",
       s_max_apdu, s_max_apdu > MAX_APDU_DEF ? " (long frames)" : "");
    for (int i = 0; i < NDEV; i++) {
        if (s_ts[i].refs) {
            char k[16];
            ts_key(s_ts[i].crc, s_ts[i].len, k);
            pf(&p, "template part %s: %u B, kept once for %u device(s)%s\n", k, (unsigned)s_ts[i].len,
               (unsigned)s_ts[i].refs, s_ts[i].saved ? "" : ", not saved yet");
        }
    }
    nvs_stats_t st;   // the flash the records take: the TULX32 has far less room for them than the TUL32
    if (nvs_get_stats(NVS_PART, &st) == ESP_OK) {
        pf(&p, "%s: %u of %u NVS entries used (32 B each), %u usable, %u namespaces\n", NVS_PART,
           (unsigned)st.used_entries, (unsigned)st.total_entries, (unsigned)(st.used_entries + st.available_entries),
           (unsigned)st.namespace_count);
    }
    xSemaphoreGive(s_mx);
    printf("%s", p.b);
    free(p.b);
}
