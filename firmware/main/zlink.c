// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dirk Tostmann
// Link between a Berry instance and a real Zigbee device. What the class sets in Zigbee2MQTT terms (dp_set) goes
// out as ZCL; attribute values the device answers or reports come back as dp_event; what the class sends towards KNX
// (ko_status, ko_send) goes to the device server (vdev.c). After each command the state is
// read back (the lamp has no reporting configured), so the class sees the real state and updates its status objects.
// Linked instances are numbered from 100 up and never meet the simulated instances 1..N of "dev N".
//
// Availability: every link follows whether its device is there (avail.h has the rules). A change
// goes to the class as dp_event "availability" on channel 0 (true/false), as Zigbee2MQTT's availability, and to the
// gateway's message object ("15.15.2 OFF", "15.15.2 ON"); while a device is off the gateway's fault object is 1.
// A device that comes back is read back, gets its binding and reporting again (a light re-interview), and its class
// gets its parameters again (zlink_params_due): what the class writes into the device did not reach it while it was
// off. After a start the bus does not know any more what it was told: the first state of each instance goes on the
// bus as a change, one instance after the other (tell_next), not only into the value KNX reads.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "app.h"
#include "avail.h"

static const char *TAG = "link";

#define NLINK KNX_VDEV_MAX   // one link per virtual device
#define FIRST_INST 100

typedef struct {
    bool used;
    int inst, k;             // Berry instance and its class
    uint16_t addr;           // Zigbee short address
    uint8_t ep;
    uint16_t trans;          // from the last 'transition' dp_set, 1/10 s; used by the next command, then cleared
    uint8_t initial;         // INI_ bits: the first value of each of these after linking is the initial read
    bool ct;                 // the class sets the colour temperature: read it back as well
    bool xy;                 // the class sets the colour (x, y): read x, y and the colour mode back as well
    esp_timer_handle_t rb;   // state read-back after a command
    esp_timer_handle_t su;   // binding and reporting set up a little after linking
    uint8_t su_try;
    uint32_t zcl_out, dp_in, knx_out;
    uint32_t reads;          // attribute reads sent (each answer counts once in dp_in)
    int64_t last_in_us;      // time of the last answer
    volatile uint32_t heard_n, fail_n;   // written by the Zigbee task: frames heard, sends that failed (avail.h)
    volatile bool nudged;    // a decision is queued already: no second one until the link task took it
    volatile bool gone;      // the device left the network (vdev.c): off, never asked; frames from its old short
                             // address do not count, another device may get that address
    av_t av;                 // availability, decided in the link task
    uint32_t n_off, n_on;    // how often it went off and came back
    bool kept_off;           // a link taken down to come back (zlink_pause): its device was off then. Read while
                             // the slot is not in use; inst and addr still say whose it was.
    int64_t params_at;       // its device answers again: the class gets its parameters at this time, 0: nothing due
} zlink_t;

static zlink_t s_l[NLINK];
static esp_timer_handle_t s_poll, s_tick, s_tell;
static int s_poll_s;
#define TICK_US 10000000   // availability is decided at least this often
// The first state of each instance after the start goes on the bus one instance after the other: 32 devices that
// answer within the same second are spread over 10 s.
#define TELL_GAP_US 300000
// A device that answers again gets its parameters after its binding and reporting (3 s + 2 s per link, EV_SETUP).
#define PARAMS_US   4000000
// Per instance (FIRST_INST + n): its availability went to the class as a change since the start. Kept while a link is
// only taken down to come back (zlink_pause), cleared when the instance goes (zlink_del).
static bool s_told[NLINK];

// TICK: all links, NUDGE: the link idx, TELL: the next held first state; DISC_*: the full interview of the device at addr
enum { EV_DP, EV_READBACK, EV_SETUP, EV_TICK, EV_NUDGE, EV_TELL, EV_DISC_START, EV_DISC_NODE, EV_DISC_EPS,
       EV_DISC_DESC };
typedef struct {
    uint8_t type, idx, ch;
    bool initial;
    uint16_t addr;     // DISC_*: the device; DISC_EPS: ch = number of endpoints in value, idx != 0: error
    int16_t err;
    char key[32];
    char value[200];   // JSON literal; DISC_EPS: the endpoints; DISC_DESC: an ep_desc_t
} lev_t;
_Static_assert(sizeof(ep_desc_t) <= 200, "an endpoint descriptor fits into an event");

static QueueHandle_t s_q;
static SemaphoreHandle_t s_mx;   // adding, moving and removing links: called from the device server, the pair task
                                 // and the console. The Berry task and the Zigbee task only look entries up.
enum { INI_MODEL = 1, INI_STATE = 2, INI_LEVEL = 4, INI_CT = 8, INI_X = 16, INI_Y = 32, INI_MODE = 64, INI_ALL = 127 };

static zlink_t *by_inst(int inst)
{
    for (int i = 0; i < NLINK; i++) {
        if (s_l[i].used && s_l[i].inst == inst) {
            return &s_l[i];
        }
    }
    return NULL;
}

static zlink_t *by_addr(uint16_t addr)
{
    for (int i = 0; i < NLINK; i++) {
        if (s_l[i].used && s_l[i].addr == addr) {
            return &s_l[i];
        }
    }
    return NULL;
}

// Whether the bus was told the availability of this instance since the start. NULL: an instance outside the
// device server's range (console tests); those are never held back.
static bool *told_of(int inst)
{
    return inst >= FIRST_INST && inst < FIRST_INST + NLINK ? &s_told[inst - FIRST_INST] : NULL;
}

static bool to_tell(const zlink_t *l)   // under the lock: its first state is known and waits for its turn
{
    const bool *t = l->used ? told_of(l->inst) : NULL;
    return t && !*t && l->av.state != AV_UNKNOWN;
}

// Basic ModelIdentifier and SWBuildID, read together wherever the model is read: a class may tell firmware versions of
// one model apart (zlink_attr_ep)
static const uint16_t IDENT[] = {0x0005, 0x4000};

#define STR_LIT_MAX 136   // a string literal in a dp_event line of 240 octets (bvm.c), quotes included

// A ZCL character string (length octet first, 0xFF: no value) as a JSON string literal, read as UTF-8 the way
// Zigbee2MQTT reads it (Node's Buffer.toString): every maximal ill-formed subsequence becomes U+FFFD. Valid
// characters from U+0080 go into the line as their UTF-8 octets, U+FFFD as EF BF BD: Berry's json.load keeps such
// octets as they are, whereas its \u decoder encodes U+007F and U+07FF overlong and knows no surrogate pairs. Control
// characters, '"' and '\' are escaped. At most `max` octets of the string are read; the literal stops before it would
// grow beyond STR_LIT_MAX characters. Returns its length.
static int zcl_str_json(const uint8_t *p, int max, char *out)
{
    int n = p[0] == 0xff ? 0 : p[0] > max ? max : p[0];
    const uint8_t *s = p + 1;
    int o = 0;
    out[o++] = '"';
    for (int i = 0; i < n;) {
        uint8_t b = s[i];
        char c[8];
        int cl = 0, used = 1;
        if (b < 0x80) {
            if (b < 0x20 || b == '"' || b == '\\') {
                cl = snprintf(c, sizeof c, "\\u%04x", b);
            } else {
                c[cl++] = (char)b;
            }
        } else {
            // the octets that may follow this lead octet (Unicode 15, table 3-7), and how many
            int need = b >= 0xC2 && b <= 0xDF ? 1 : b >= 0xE0 && b <= 0xEF ? 2 : b >= 0xF0 && b <= 0xF4 ? 3 : 0;
            uint8_t lo = b == 0xE0 ? 0xA0 : b == 0xF0 ? 0x90 : 0x80;
            uint8_t hi = b == 0xED ? 0x9F : b == 0xF4 ? 0x8F : 0xBF;
            int k = 0;
            while (k < need && i + 1 + k < n && s[i + 1 + k] >= lo && s[i + 1 + k] <= hi) {
                lo = 0x80;
                hi = 0xBF;
                k++;
            }
            used = 1 + k;
            if (need && k == need) {
                memcpy(c, s + i, (size_t)used);
                cl = used;
            } else {   // the lead and the continuation octets that fitted: one U+FFFD; the next octet starts anew
                memcpy(c, "\xEF\xBF\xBD", 3);
                cl = 3;
            }
        }
        if (o + cl + 1 > STR_LIT_MAX) {
            break;
        }
        memcpy(out + o, c, (size_t)cl);
        o += cl;
        i += used;
    }
    out[o++] = '"';
    out[o] = 0;
    return o;
}

// Value of "name" in the flat JSON object the prelude's _emit writes: a string without its quotes, or the literal of
// a number or bool. Key order in Berry's json.dump is not fixed, so look the name up wherever it is.
static bool jget(const char *js, const char *name, char *out, size_t n)
{
    char pat[32];
    snprintf(pat, sizeof pat, "\"%s\"", name);
    for (const char *p = strstr(js, pat); p; p = strstr(p + 1, pat)) {
        const char *q = p + strlen(pat);
        while (*q == ' ') {
            q++;
        }
        if (*q != ':') {
            continue;   // the name stood as a value somewhere, not as a key
        }
        q++;
        while (*q == ' ') {
            q++;
        }
        size_t i = 0;
        if (*q == '"') {
            for (q++; *q && *q != '"' && i + 1 < n; q++) {
                if (*q == '\\' && q[1]) {
                    q++;
                }
                out[i++] = *q;
            }
        } else {
            while (*q && *q != ',' && *q != '}' && *q != ' ' && i + 1 < n) {
                out[i++] = *q++;
            }
        }
        out[i] = 0;
        return i > 0;
    }
    return false;
}

static void rb_fire(void *arg)
{
    lev_t ev = {.type = EV_READBACK, .idx = (uint8_t)(uintptr_t)arg};
    if (s_q) {
        xQueueSend(s_q, &ev, 0);
    }
}

static void su_fire(void *arg)
{
    lev_t ev = {.type = EV_SETUP, .idx = (uint8_t)(uintptr_t)arg};
    if (s_q) {
        xQueueSend(s_q, &ev, 0);
    }
}

static void readback(zlink_t *l, uint16_t tenths)
{
    esp_timer_stop(l->rb);
    esp_timer_start_once(l->rb, (uint64_t)tenths * 100000 + 700000);   // after the transition, plus 0.7 s
}

static int pick(const char *v, const char *const *names, const int *vals, int n, int dflt)
{
    for (int i = 0; i < n; i++) {
        if (strcmp(v, names[i]) == 0) {
            return vals[i];
        }
    }
    return dflt;
}

// one Zigbee2MQTT property set by the class -> one ZCL command
static void dp_set(zlink_t *l, const char *key, const char *val)
{
    char what[72];
    esp_err_t e;
    bool rb = true;
    if (strcmp(key, "transition") == 0) {   // seconds, applies to the next command
        float s = strtof(val, NULL);
        l->trans = s <= 0 ? 0 : s >= 6553 ? 65530 : (uint16_t)(s * 10 + 0.5f);
        return;
    }
    uint16_t t = l->trans;
    l->trans = 0;
    if (strcmp(key, "state") == 0) {
        static const char *const n[] = {"OFF", "ON", "TOGGLE"};
        static const int v[] = {0, 1, 2};
        int w = pick(val, n, v, 3, -1);
        if (w < 0) {
            goto unmapped;
        }
        e = zb_onoff(l->addr, l->ep, w);
        snprintf(what, sizeof what, "On/Off %s", n[w]);
    } else if (strcmp(key, "brightness") == 0) {
        int b = atoi(val);
        b = b < 0 ? 0 : b > 254 ? 254 : b;
        e = zb_level(l->addr, l->ep, (uint8_t)b, t);
        snprintf(what, sizeof what, "Move to Level with On/Off %d in %u/10 s", b, t);
    } else if (strcmp(key, "brightness_move") == 0 || strcmp(key, "brightness_move_onoff") == 0) {
        bool onoff = key[15] != 0;
        if (strcmp(val, "stop") == 0) {
            e = zb_level_stop(l->addr, l->ep, onoff);
            snprintf(what, sizeof what, "Stop%s", onoff ? " with On/Off" : "");
        } else {
            int r = atoi(val);
            int rate = abs(r);
            rate = rate < 1 ? 1 : rate > 254 ? 254 : rate;
            e = zb_level_move(l->addr, l->ep, r >= 0, (uint8_t)rate, onoff);
            snprintf(what, sizeof what, "Move%s %s, %d/s", onoff ? " with On/Off" : "", r >= 0 ? "up" : "down", rate);
            rb = false;   // read back after the stop
        }
    } else if (strcmp(key, "color_xy") == 0) {   // "X,Y" raw 0..65279 (CIE x, y times 65535), C005 datapoints
        unsigned x, y;
        if (sscanf(val, "%u,%u", &x, &y) != 2) {
            goto unmapped;
        }
        x = x > 0xFEFF ? 0xFEFF : x;
        y = y > 0xFEFF ? 0xFEFF : y;
        l->xy = true;
        e = zb_color_xy(l->addr, l->ep, (uint16_t)x, (uint16_t)y, t);
        snprintf(what, sizeof what, "Move to Color x %u y %u in %u/10 s", x, y, t);
    } else if (strcmp(key, "color_temp") == 0) {   // mired
        int m = atoi(val);
        m = m < 1 ? 1 : m > 0xFEFF ? 0xFEFF : m;
        l->ct = true;
        e = zb_color_temp(l->addr, l->ep, (uint16_t)m, t);
        snprintf(what, sizeof what, "Move to Color Temperature %d mired in %u/10 s", m, t);
    } else if (strcmp(key, "color_temp_move") == 0) {   // mired per second, > 0 warmer, < 0 cooler, or "stop"
        l->ct = true;
        if (strcmp(val, "stop") == 0) {
            e = zb_color_temp_move(l->addr, l->ep, 0, 0);
            snprintf(what, sizeof what, "Stop Move Color Temperature");
        } else {
            int r = atoi(val);
            int rate = abs(r);
            rate = rate < 1 ? 1 : rate > 0xFFFF ? 0xFFFF : rate;
            e = zb_color_temp_move(l->addr, l->ep, r >= 0 ? 1 : -1, (uint16_t)rate);
            snprintf(what, sizeof what, "Move Color Temperature %s, %d mired/s", r >= 0 ? "up (warmer)" : "down (cooler)",
                     rate);
            rb = false;   // read back after the stop
        }
    } else if (strcmp(key, "color_temp_startup") == 0) {   // Color Control StartUpColorTemperatureMireds (uint16)
        int m = atoi(val);   // 65535: the previous value
        m = m < 1 ? 1 : m > 0xFFFF ? 0xFFFF : m;
        e = zb_write_u16(l->addr, l->ep, 0x0300, 0x4010, 0x21, (uint16_t)m);
        snprintf(what, sizeof what, "write Color StartUpColorTemperatureMireds = %d", m);
        rb = false;
    } else if (strcmp(key, "zcl.write") == 0) {
        // The template says what to write: "cluster,attribute,type,manufacturer,value" in hex, manufacturer 0 for a
        // standard attribute. The knowledge about the device stays in the template, not here.
        unsigned cl, at, ty, mf;
        unsigned long v;
        int size = 0;
        if (sscanf(val, "%x,%x,%x,%x,%lx", &cl, &at, &ty, &mf, &v) == 5) {
            switch (ty) {
            case 0x10: case 0x18: case 0x20: case 0x28: case 0x30: size = 1; break;   // bool, map8, u8, s8, enum8
            case 0x19: case 0x21: case 0x29: case 0x31: size = 2; break;              // map16, u16, s16, enum16
            case 0x23: case 0x2b: size = 4; break;                                    // u32, s32
            default: break;
            }
        }
        if (!size || cl > 0xFFFF || at > 0xFFFF || mf > 0xFFFF) {
            goto unmapped;
        }
        e = zb_write_attr(l->addr, l->ep, (uint16_t)cl, (uint16_t)at, (uint8_t)ty, (uint32_t)v, (uint8_t)size,
                          (uint16_t)mf);
        snprintf(what, sizeof what, "write 0x%04x/0x%04x type 0x%02x = 0x%lx%s", cl, at, ty, v,
                 mf ? " (manufacturer-specific)" : "");
        rb = false;
    } else if (strcmp(key, "identify") == 0) {
        e = zb_identify(l->addr, l->ep, 3);
        snprintf(what, sizeof what, "Identify 3 s");
        rb = false;
    } else if (strcmp(key, "power_on_behavior") == 0) {   // On/Off StartUpOnOff (enum8)
        static const char *const n[] = {"off", "on", "toggle", "previous"};
        static const int v[] = {0x00, 0x01, 0x02, 0xff};
        int w = pick(val, n, v, 4, -1);
        if (w < 0) {
            goto unmapped;
        }
        e = zb_write_u8(l->addr, l->ep, 0x0006, 0x4003, 0x30, (uint8_t)w);
        snprintf(what, sizeof what, "write On/Off StartUpOnOff = 0x%02x", w);
        rb = false;
    } else if (strcmp(key, "level_config.current_level_startup") == 0) {   // Level StartUpCurrentLevel (uint8)
        static const char *const n[] = {"minimum", "previous"};
        static const int v[] = {0x00, 0xff};
        int w = pick(val, n, v, 2, -1);
        if (w < 0) {
            w = atoi(val);
            w = w < 1 ? 1 : w > 254 ? 254 : w;
        }
        e = zb_write_u8(l->addr, l->ep, 0x0008, 0x4000, 0x20, (uint8_t)w);
        snprintf(what, sizeof what, "write Level StartUpCurrentLevel = 0x%02x", w);
        rb = false;
    } else {
        goto unmapped;
    }
    l->zcl_out++;
    ESP_LOGI(TAG, "inst %d -> 0x%04x: %s = %s => %s: %s", l->inst, l->addr, key, val, what, esp_err_to_name(e));
    if (e == ESP_OK && rb) {
        readback(l, t);
    }
    return;
unmapped:
    ESP_LOGI(TAG, "inst %d: %s = %s has no ZCL mapping", l->inst, key, val);
}

void zlink_out(const char *js)
{
    char v[48], op[16];
    if (!jget(js, "inst", v, sizeof v)) {
        return;   // 'ready', 'done': not from an instance
    }
    zlink_t *l = by_inst(atoi(v));
    if (!l || !jget(js, "op", op, sizeof op)) {
        return;   // a simulated instance: only counted in bvm.c
    }
    if (strcmp(op, "dp_set") == 0) {
        char key[40];
        if (jget(js, "key", key, sizeof key) && jget(js, "value", v, sizeof v)) {
            dp_set(l, key, v);
        }
    } else if (strcmp(op, "ko_status") == 0 || strcmp(op, "ko_send") == 0) {
        char ko[8] = "?", raw[32] = "", ini[8] = "";
        jget(js, "ko", ko, sizeof ko);
        jget(js, "raw", raw, sizeof raw);
        jget(js, "initial", ini, sizeof ini);
        l->knx_out++;
        bool initial = strcmp(ini, "true") == 0;
        ESP_LOGI(TAG, "inst %d -> KNX: %s KO %s = %s%s", l->inst, op, ko, raw,
                 initial ? " (initial: shadow only, not sent)" : "");
        uint8_t val[KNX_KO_VAL];
        size_t hl = strlen(raw);
        if (hl >= 2 && hl <= 2 * KNX_KO_VAL && hl % 2 == 0) {   // to the device server: it knows the group address
            for (size_t i = 0; i < hl / 2; i++) {
                char two[3] = {raw[2 * i], raw[2 * i + 1], 0};
                val[i] = (uint8_t)strtoul(two, NULL, 16);
            }
            vdev_ko_out(l->inst, atoi(ko), val, (int)(hl / 2), initial, strcmp(op, "ko_status") == 0);
        }
    } else {
        ESP_LOGI(TAG, "inst %d: %s", l->inst, js);
    }
}

// called in the Zigbee task: format, queue, never block
// called in the Zigbee task: format, queue, never block
bool zlink_cmd(uint16_t addr, uint16_t cluster, uint8_t cmd, const uint8_t *payload, int len)
{
    zlink_t *l = by_addr(addr);
    if (!l || !s_q) {
        return false;
    }
    lev_t ev = {.type = EV_DP, .idx = (uint8_t)(l - s_l), .ch = 1};
    strcpy(ev.key, "zcl.cmd");
    // "cluster,command,payload" in hex, as a JSON string: the template takes it apart
    int o = snprintf(ev.value, sizeof ev.value, "\"%04x,%02x,", cluster, cmd);
    for (int i = 0; i < len && o < (int)sizeof ev.value - 4; i++) {
        o += snprintf(ev.value + o, sizeof ev.value - o, "%02x", payload[i]);
    }
    snprintf(ev.value + o, sizeof ev.value - o, "\"");
    if (xQueueSend(s_q, &ev, 0) != pdTRUE) {
        ESP_LOGW(TAG, "queue full, command 0x%04x/0x%02x from 0x%04x dropped", cluster, cmd, addr);
    }
    return true;
}

// ep: the endpoint the value came from, 0: not known (the link's endpoint stands for it)
void zlink_attr_ep(uint16_t addr, uint8_t ep, uint16_t cluster, uint16_t attr, uint8_t type, const void *val)
{
    zlink_t *l = by_addr(addr);
    const uint8_t *p = val;
    if (!l || !p || !s_q) {
        return;
    }
    lev_t ev = {.type = EV_DP, .idx = (uint8_t)(l - s_l), .ch = 1};
    uint8_t bit = 0;
    if (cluster == 0x0000 && attr == 0x0005 && type == 0x42) {   // ModelIdentifier -> identity on endpoint 0
        ev.ch = 0;
        bit = INI_MODEL;
        strcpy(ev.key, "zigbeeModel");
        zcl_str_json(p, 32, ev.value);   // IKEA has models with a control character in front
    } else if (cluster == 0x0000 && attr == 0x4000 && type == 0x42) {
        // SWBuildID: the device's firmware version, for classes whose devices changed what they report with it.
        // Fixed while linked: always initial.
        ev.ch = 0;
        strcpy(ev.key, "swBuildId");
        zcl_str_json(p, 32, ev.value);
        ev.initial = true;
        if (xQueueSend(s_q, &ev, 0) != pdTRUE) {
            ESP_LOGW(TAG, "queue full, %s from 0x%04x dropped", ev.key, addr);
        }
        return;
    } else if (cluster == 0x0006 && attr == 0x0000 && type == 0x10) {
        bit = INI_STATE;
        strcpy(ev.key, "state");
        strcpy(ev.value, p[0] ? "\"ON\"" : "\"OFF\"");
    } else if (cluster == 0x0008 && attr == 0x0000 && type == 0x20) {
        bit = INI_LEVEL;
        strcpy(ev.key, "brightness");
        snprintf(ev.value, sizeof ev.value, "%u", p[0]);
    } else if (cluster == 0x0300 && attr == 0x0007 && type == 0x21) {
        bit = INI_CT;
        strcpy(ev.key, "color_temp");
        snprintf(ev.value, sizeof ev.value, "%u", (unsigned)(p[0] | p[1] << 8));
    } else if (cluster == 0x0300 && (attr == 0x0003 || attr == 0x0004) && type == 0x21) {   // CurrentX, CurrentY
        bit = attr == 0x0003 ? INI_X : INI_Y;
        strcpy(ev.key, attr == 0x0003 ? "color_x" : "color_y");
        snprintf(ev.value, sizeof ev.value, "%u", (unsigned)(p[0] | p[1] << 8));
    } else if (cluster == 0x0300 && attr == 0x0008 && type == 0x30) {   // ColorMode, as Zigbee2MQTT names it
        static const char *const MODE[] = {"hs", "xy", "color_temp"};
        if (p[0] > 2) {
            return;
        }
        bit = INI_MODE;
        strcpy(ev.key, "color_mode");
        snprintf(ev.value, sizeof ev.value, "\"%s\"", MODE[p[0]]);
    } else if (cluster == 0x0300 && attr == 0x400A && type == 0x19) {   // ColorCapabilities: fixed, always initial
        strcpy(ev.key, "colorCapabilities");
        snprintf(ev.value, sizeof ev.value, "%u", (unsigned)(p[0] | p[1] << 8));
        ev.initial = true;
        if (xQueueSend(s_q, &ev, 0) != pdTRUE) {
            ESP_LOGW(TAG, "queue full, %s from 0x%04x dropped", ev.key, addr);
        }
        return;
    } else if (cluster == 0x0001 && attr == 0x0021 && type == 0x20) {
        // BatteryPercentageRemaining as the device sends it, 0..200 in half percent by the ZCL; some firmware sends
        // whole percent, so the class decides (with swBuildId). On the endpoint it came from. 0xFF: not known, nothing
        // to tell. Not an initial value: a battery device answers a read when it is awake, and its status object may
        // go out then.
        if (p[0] == 0xff) {
            return;
        }
        ev.ch = ep ? ep : l->ep;
        strcpy(ev.key, "batteryPercentageRemaining");
        snprintf(ev.value, sizeof ev.value, "%u", p[0]);
    } else if (cluster == 0x0300 && (attr == 0x400B || attr == 0x400C) && type == 0x21) {
        // the range the lamp can do; the class prefers it to its model table. Fixed values: always initial.
        strcpy(ev.key, attr == 0x400B ? "colorTempPhysicalMin" : "colorTempPhysicalMax");
        snprintf(ev.value, sizeof ev.value, "%u", (unsigned)(p[0] | p[1] << 8));
        ev.initial = true;
        if (xQueueSend(s_q, &ev, 0) != pdTRUE) {
            ESP_LOGW(TAG, "queue full, %s from 0x%04x dropped", ev.key, addr);
        }
        return;
    } else {
        return;
    }
    ev.initial = l->initial & bit;
    l->initial &= (uint8_t)~bit;
    if (xQueueSend(s_q, &ev, 0) != pdTRUE) {
        ESP_LOGW(TAG, "queue full, %s from 0x%04x dropped", ev.key, addr);
    }
}

void zlink_attr(uint16_t addr, uint16_t cluster, uint16_t attr, uint8_t type, const void *val)
{
    zlink_attr_ep(addr, 0, cluster, attr, type, val);
}

// ---------------------------------------------------------------- availability (avail.h)
typedef struct {
    int idx, inst, k;
    uint16_t addr;
    uint8_t ep, from, to;
    bool ping;
    bool hold;            // the first state since the start: the class gets it at its turn (tell_next)
    uint32_t fails;
    int64_t quiet_s;
} av_act_t;

// Decide for every link (only < 0) or one, under the lock; act on the outcome after it (Berry, the device server and
// the Zigbee stack must not be asked while the lock is held: the device server takes its own lock first).
static void avail_run(int only)
{
    static av_act_t act[NLINK];   // the link task's own: one call at a time
    static const uint16_t zv[] = {0x0000};   // Basic ZCLVersion: every device has it
    int n = 0;
    bool tell = false;
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_mx, portMAX_DELAY);
    for (int i = 0; i < NLINK; i++) {
        zlink_t *l = &s_l[i];
        if (!l->used || (only >= 0 && i != only)) {
            continue;
        }
        l->nudged = false;
        uint8_t from = l->av.state;
        bool ping;
        uint8_t to = av_step(&l->av, l->heard_n, l->fail_n, now, &ping);
        if (l->gone) {   // it left: off, and not asked under an address another device may have now
            to = l->av.state = AV_OFF;
            ping = false;
        }
        if (to != from || ping) {
            // The first state of an instance the bus has not been told since the start waits for its turn. Any
            // later change goes to the class at once and counts as told.
            bool *t = told_of(l->inst);
            bool hold = to != from && from == AV_UNKNOWN && t && !*t;
            if (to != from && !hold && t) {
                *t = true;
            }
            act[n++] = (av_act_t){.idx = i, .inst = l->inst, .k = l->k, .addr = l->addr, .ep = l->ep, .from = from,
                                  .to = to, .ping = ping, .hold = hold, .fails = l->fail_n - l->av.fail_base,
                                  .quiet_s = (now - l->av.last_us) / 1000000};
            if (to != from && to == AV_OFF) {
                l->n_off++;
                l->params_at = 0;   // nothing to write into a device that does not answer
            } else if (to != from && from == AV_OFF) {
                l->n_on++;
                l->params_at = now + PARAMS_US + (int64_t)i * 2000000;   // after its binding and reporting
            }
        }
    }
    for (int i = 0; i < NLINK && !tell; i++) {
        tell = to_tell(&s_l[i]);
    }
    xSemaphoreGive(s_mx);
    if (tell) {
        esp_timer_start_once(s_tell, TELL_GAP_US);   // running already: it fires anyway, and tell_next() goes on
    }
    for (int j = 0; j < n; j++) {
        const av_act_t *a = &act[j];
        if (a->ping) {
            esp_err_t e = zb_read(a->addr, a->ep, 0x0000, zv, 1);
            s_l[a->idx].reads += e == ESP_OK;
            ESP_LOGI(TAG, "inst %d -> 0x%04x: quiet for %lld s, asked (%s)", a->inst, a->addr, (long long)a->quiet_s,
                     esp_err_to_name(e));
        }
        if (a->to == a->from) {
            continue;
        }
        bool on = a->to == AV_ON, first = a->from == AV_UNKNOWN;
        if (on) {
            ESP_LOGI(TAG, "inst %d <-> 0x%04x: %s", a->inst, a->addr, first ? "answers" : "answers again");
        } else {
            ESP_LOGW(TAG, "inst %d <-> 0x%04x: NOT reachable (%lu failed sends, nothing heard for %lld s)", a->inst,
                     a->addr, (unsigned long)a->fails, (long long)a->quiet_s);
        }
        // to the class, as Zigbee2MQTT's availability; the first "true" after linking again (the bus was told since
        // the start) is the initial value
        if (a->k >= 0 && !a->hold) {
            bvm_line(a->k, "{\"call\":\"dp_event\",\"inst\":%d,\"id\":%lu,\"ch\":0,\"key\":\"availability\","
                     "\"value\":%s,\"initial\":%s}", a->inst, bvm_next_id(), on ? "true" : "false",
                     on && first ? "true" : "false");
        }
        if (!(on && first)) {   // that a device answers after linking is nothing to tell
            vdev_avail_note(a->inst, on);
        }
        if (on && !first) {   // back: what it shows now may not be what KNX was told, and its setup may be gone
            // One device after the other, as after linking: lamps of one circuit come back together, and with all
            // asked at once requests got lost (2026-11-12)
            zlink_t *l = &s_l[a->idx];
            readback(l, (uint16_t)(13 + 10 * a->idx));
            l->su_try = 0;
            esp_timer_stop(l->su);
            esp_timer_start_once(l->su, 3000000 + (uint64_t)a->idx * 2000000);
        }
    }
}

static void nudge(const zlink_t *l)   // the Zigbee task: a decision may be due now, not only at the next tick
{
    lev_t ev = {.type = EV_NUDGE, .idx = (uint8_t)(l - s_l)};
    if (s_q) {
        xQueueSend(s_q, &ev, 0);   // full: the next tick decides
    }
}

static void tick_fire(void *arg)
{
    (void)arg;
    lev_t ev = {.type = EV_TICK};
    if (s_q) {
        xQueueSend(s_q, &ev, 0);
    }
}

static void tell_fire(void *arg)
{
    (void)arg;
    lev_t ev = {.type = EV_TELL};
    if (s_q) {
        xQueueSend(s_q, &ev, 0);   // full: the next tick arms the timer again
    }
}

// The first state of one instance since the start, held back by avail_run(), goes to the class as a change: the
// status object goes on the bus, also when it is the "1" a lamp that answers brings. Before a restart the bus may have
// been told "0" (and a visualisation would show "not reachable" until the lamp failed once more). The next instance
// follows TELL_GAP_US later.
static void tell_next(void)
{
    int k = -1, inst = 0;
    bool on = false, more = false;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    for (int i = 0; i < NLINK; i++) {
        zlink_t *l = &s_l[i];
        if (!to_tell(l)) {
            continue;
        }
        if (k >= 0) {
            more = true;
            break;
        }
        *told_of(l->inst) = true;
        k = l->k;
        inst = l->inst;
        on = l->av.state == AV_ON;
    }
    xSemaphoreGive(s_mx);
    if (k >= 0) {
        ESP_LOGI(TAG, "inst %d: %s, the first state since the start goes on the bus", inst,
                 on ? "reachable" : "NOT reachable");
        bvm_line(k, "{\"call\":\"dp_event\",\"inst\":%d,\"id\":%lu,\"ch\":0,\"key\":\"availability\",\"value\":%s,"
                 "\"initial\":false}", inst, bvm_next_id(), on ? "true" : "false");
    }
    if (more) {
        esp_timer_start_once(s_tell, TELL_GAP_US);
    }
}

// From the device server, which hands the class its parameters when this says so: once per return of the device
// ("answers again"), PARAMS_US plus 2 s per link after it, so that lamps of one circuit do not all write at once.
bool zlink_params_due(int inst)
{
    const zlink_t *c = by_inst(inst);   // a first look without the lock, as zlink_is_off(): nearly always nothing
    if (!s_mx || !c || !c->params_at) {
        return false;
    }
    bool due = false;
    uint16_t addr = 0;
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_mx, portMAX_DELAY);
    zlink_t *l = by_inst(inst);
    if (l && l->params_at && now >= l->params_at) {
        l->params_at = 0;
        addr = l->addr;
        due = true;
    }
    xSemaphoreGive(s_mx);
    if (due) {
        ESP_LOGI(TAG, "inst %d <-> 0x%04x: answers again, its class gets its parameters again", inst, addr);
    }
    return due;
}

// called in the Zigbee task: a frame came from this short address (ZCL, device announce)
void zlink_heard(uint16_t addr)
{
    zlink_t *l = by_addr(addr);
    if (l && !l->gone) {
        l->heard_n++;
        if (l->av.state != AV_ON && !l->nudged) {   // one queued decision per link is enough
            l->nudged = true;
            nudge(l);
        }
    }
}

// called in the Zigbee task: the APS confirm of a unicast to this short address
void zlink_sent(uint16_t addr, uint8_t status)
{
    zlink_t *l = status ? by_addr(addr) : NULL;
    if (l && !l->gone) {
        l->fail_n++;
        if (!l->av.sleepy && l->av.state != AV_OFF && !l->nudged) {
            l->nudged = true;
            nudge(l);
        }
    }
}

void zlink_sleepy(int inst, bool sleepy)   // from the device server: the MAC capability of the device's announce
{
    if (!s_mx) {
        return;
    }
    xSemaphoreTake(s_mx, portMAX_DELAY);
    zlink_t *l = by_inst(inst);
    if (l) {
        l->av.sleepy = sleepy;
    }
    xSemaphoreGive(s_mx);
}

void zlink_gone(int inst, bool gone)   // from the device server: its device left the network, or is back
{
    if (!s_mx) {
        return;
    }
    xSemaphoreTake(s_mx, portMAX_DELAY);
    zlink_t *l = by_inst(inst);
    if (l && l->gone != gone) {
        l->gone = gone;
        if (!gone) {
            l->heard_n++;   // back at its address: its announce is what brought it
        }
        if (!l->nudged) {
            l->nudged = true;
            nudge(l);
        }
    }
    xSemaphoreGive(s_mx);
}

bool zlink_is_off(int inst)   // without the lock: one field, read by the device server's timers
{
    const zlink_t *l = by_inst(inst);
    return l && l->av.state == AV_OFF;
}

int zlink_unavailable(void)   // linked devices that are off now, and those whose link is only taken down for a moment
{
    int n = 0;
    for (int i = 0; i < NLINK; i++) {
        n += s_l[i].used ? s_l[i].av.state == AV_OFF : s_l[i].kept_off;
    }
    return n;
}

// ---------------------------------------------------------------- the full interview (ep_pick.h)
// One device at a time, in the link task: Active_EP_req, then Simple_Desc_req per endpoint, then the rule of
// ep_pick.h; the endpoint goes to the device server, which keeps it and moves the link there. Each answer is logged,
// so the console shows what a new device brings (endpoints, profile, clusters).
#define DISC_STEP_US 15000000   // an answer that does not come within this ends the interview with what is known
static struct {
    bool busy, incomplete;     // incomplete: an answer failed or did not come; the endpoint is not changed then
    uint16_t addr;
    uint8_t step;              // 0 node descriptor, 1 active endpoints, 2 simple descriptors
    uint8_t eps[EP_DESC_MAX];
    uint8_t want;              // the endpoint whose simple descriptor is asked for now
    int n, i, nd;
    ep_desc_t d[EP_DESC_MAX];
    int64_t deadline;
} s_disc;
static uint16_t s_disc_wait[4];
static int s_disc_nwait;

static void disc_start(uint16_t addr);
static void disc_eps_ask(void);

// The endpoint changes only after a complete interview and only to one that scores higher than the one the device
// has: a correctly set device must not move because one answer was lost.
static void disc_end(const char *why)
{
    uint8_t ep = ep_pick(s_disc.d, s_disc.nd), cur = vdev_endpoint_of(s_disc.addr);
    int score = -1, cur_score = -1;
    for (int i = 0; i < s_disc.nd; i++) {
        if (s_disc.d[i].ep == ep) {
            score = ep_score(&s_disc.d[i]);
        }
        if (s_disc.d[i].ep == cur) {
            cur_score = ep_score(&s_disc.d[i]);
        }
    }
    char list[4 * EP_DESC_MAX + 1] = "";
    for (int i = 0; i < s_disc.n; i++) {
        snprintf(list + strlen(list), sizeof list - strlen(list), "%s%u", i ? "," : "", s_disc.eps[i]);
    }
    bool move = ep && ep != cur && !s_disc.incomplete && score > cur_score;
    if (move) {
        ESP_LOGI(TAG, "interview 0x%04x: %s; endpoints %s, %d described, takes endpoint %u (score %d, endpoint %u "
                 "had %d)", s_disc.addr, why, list[0] ? list : "-", s_disc.nd, ep, score, cur, cur_score);
    } else {
        ESP_LOGI(TAG, "interview 0x%04x: %s; endpoints %s, %d described; keeps endpoint %u (%s)", s_disc.addr, why,
                 list[0] ? list : "-", s_disc.nd, cur,
                 s_disc.incomplete ? "the interview is not complete"
                 : !ep             ? "none fits"
                 : ep == cur       ? "it is the best one"
                                   : "no endpoint scores higher");
    }
    uint16_t addr = s_disc.addr;
    s_disc.busy = false;
    if (move) {
        vdev_set_endpoint(addr, ep);
    }
    if (s_disc_nwait) {
        uint16_t next = s_disc_wait[0];
        memmove(s_disc_wait, s_disc_wait + 1, (size_t)(--s_disc_nwait) * sizeof s_disc_wait[0]);
        disc_start(next);
    }
}

static void disc_next(void)   // the next simple descriptor, or the end
{
    while (s_disc.i < s_disc.n) {
        uint8_t ep = s_disc.eps[s_disc.i];
        if (ep >= 1 && ep <= 240) {   // 242 Green Power, 0 the ZDO itself: nothing a class talks to
            esp_err_t e = zb_simple_desc(s_disc.addr, ep);
            if (e == ESP_OK) {
                s_disc.want = ep;
                s_disc.deadline = esp_timer_get_time() + DISC_STEP_US;
                return;
            }
            s_disc.incomplete = true;
            ESP_LOGW(TAG, "interview 0x%04x: Simple_Desc_req for endpoint %u: %s", s_disc.addr, ep, esp_err_to_name(e));
        }
        s_disc.i++;
    }
    disc_end("done");
}

static void disc_start(uint16_t addr)
{
    if (s_disc.busy) {
        bool there = addr == s_disc.addr;
        for (int i = 0; i < s_disc_nwait; i++) {
            there = there || s_disc_wait[i] == addr;
        }
        if (!there && s_disc_nwait < (int)(sizeof s_disc_wait / sizeof s_disc_wait[0])) {
            s_disc_wait[s_disc_nwait++] = addr;
        }
        return;
    }
    memset(&s_disc, 0, sizeof s_disc);
    s_disc.busy = true;
    s_disc.addr = addr;
    // the node descriptor first: its MAC capability says whether the device sleeps, also for a device whose record
    // has none (paired with an earlier development version, or its announce was lost)
    esp_err_t e = zb_node_desc(addr);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "interview 0x%04x: Node_Desc_req: %s", addr, esp_err_to_name(e));
        disc_eps_ask();
        return;
    }
    s_disc.deadline = esp_timer_get_time() + DISC_STEP_US;
    ESP_LOGI(TAG, "interview 0x%04x: asks for its node descriptor and endpoints", addr);
}

static void disc_eps_ask(void)
{
    s_disc.step = 1;
    esp_err_t e = zb_active_ep(s_disc.addr);
    if (e != ESP_OK) {
        s_disc.incomplete = true;
        disc_end(esp_err_to_name(e));
        return;
    }
    s_disc.deadline = esp_timer_get_time() + DISC_STEP_US;
}

static void disc_event(const lev_t *ev)
{
    if (ev->type == EV_DISC_START) {
        disc_start(ev->addr);
        return;
    }
    if (!s_disc.busy || ev->addr != s_disc.addr) {
        return;   // an answer late for an interview that has ended
    }
    if (ev->type == EV_DISC_NODE) {
        if (s_disc.step != 0) {
            return;   // late or twice
        }
        if (ev->err) {
            ESP_LOGW(TAG, "interview 0x%04x: no node descriptor (error %d)", s_disc.addr, ev->err);
        } else {
            uint8_t cap = (uint8_t)ev->value[0];
            ESP_LOGI(TAG, "interview 0x%04x: MAC capability 0x%02x (receiver %s when idle)", s_disc.addr, cap,
                     cap & 0x08 ? "on" : "off");
            vdev_set_capability_of(s_disc.addr, cap);
        }
        disc_eps_ask();
    } else if (ev->type == EV_DISC_EPS) {
        if (s_disc.step != 1) {
            return;   // late or twice
        }
        if (ev->err) {
            char why[40];
            snprintf(why, sizeof why, "no endpoint list (error %d)", ev->err);
            s_disc.incomplete = true;
            disc_end(why);
            return;
        }
        s_disc.step = 2;
        s_disc.n = ev->ch;
        memcpy(s_disc.eps, ev->value, (size_t)s_disc.n);
        s_disc.i = 0;
        disc_next();
    } else if (ev->type == EV_DISC_DESC) {
        ep_desc_t d;
        memcpy(&d, ev->value, sizeof d);
        if (s_disc.step != 2 || d.ep != s_disc.want) {   // late, twice, or for another endpoint: not the one asked
            ESP_LOGW(TAG, "interview 0x%04x: answer for endpoint %u ignored (waiting for %u)", s_disc.addr, d.ep,
                     s_disc.want);
            return;
        }
        s_disc.want = 0;
        if (ev->err) {
            s_disc.incomplete = true;
        }
        if (!ev->err && s_disc.nd < EP_DESC_MAX) {
            s_disc.d[s_disc.nd++] = d;
            char h[6 * EP_CL_MAX + 16];
            int o = snprintf(h, sizeof h, "in");
            for (int i = 0; i < d.nin + d.nout && o < (int)sizeof h - 8; i++) {
                o += snprintf(h + o, sizeof h - o, "%s %04x", i == d.nin ? " | out" : "", d.cl[i]);
            }
            ESP_LOGI(TAG, "interview 0x%04x ep %u: profile 0x%04x, %s (score %d)", s_disc.addr, d.ep, d.profile, h,
                     ep_score(&d));
        } else if (ev->err) {
            ESP_LOGW(TAG, "interview 0x%04x ep %u: no simple descriptor (error %d)", s_disc.addr, d.ep, ev->err);
        }
        s_disc.i++;
        disc_next();
    }
}

static void disc_post(const lev_t *ev)
{
    if (!s_q || xQueueSend(s_q, ev, 0) != pdTRUE) {
        ESP_LOGW(TAG, "interview 0x%04x: queue full, an answer is lost (the interview ends on its timeout)", ev->addr);
    }
}

void zlink_discover(uint16_t addr)
{
    lev_t ev = {.type = EV_DISC_START, .addr = addr};
    disc_post(&ev);
}

void zlink_disc_node(uint16_t addr, uint8_t capability, int err)   // in the Zigbee task: queue
{
    lev_t ev = {.type = EV_DISC_NODE, .addr = addr, .err = (int16_t)err};
    ev.value[0] = (char)capability;
    disc_post(&ev);
}

void zlink_disc_eps(uint16_t addr, const uint8_t *eps, int n, int err)   // in the Zigbee task: copy and queue
{
    lev_t ev = {.type = EV_DISC_EPS, .addr = addr, .err = (int16_t)err};
    n = n < 0 ? 0 : n > EP_DESC_MAX ? EP_DESC_MAX : n;
    if (eps && !err) {
        memcpy(ev.value, eps, (size_t)n);
        ev.ch = (uint8_t)n;
    }
    disc_post(&ev);
}

void zlink_disc_desc(uint16_t addr, const ep_desc_t *d, int err)   // in the Zigbee task: copy and queue
{
    lev_t ev = {.type = EV_DISC_DESC, .addr = addr, .err = (int16_t)err};
    memcpy(ev.value, d, sizeof *d);
    disc_post(&ev);
}

static void link_task(void *arg)
{
    (void)arg;
    static const uint16_t a0[] = {0x0000};
    lev_t ev;
    for (;;) {
        xQueueReceive(s_q, &ev, portMAX_DELAY);
        if (ev.type == EV_TELL) {
            tell_next();
            continue;
        }
        if (ev.type == EV_TICK || ev.type == EV_NUDGE) {
            avail_run(ev.type == EV_NUDGE ? ev.idx : -1);
            if (s_disc.busy && esp_timer_get_time() > s_disc.deadline) {
                if (s_disc.step == 0) {   // no node descriptor: the endpoints are what counts, ask for them
                    ESP_LOGW(TAG, "interview 0x%04x: no node descriptor in time", s_disc.addr);
                    disc_eps_ask();
                } else {
                    s_disc.incomplete = true;
                    disc_end("no answer in time");
                }
            }
            continue;
        }
        if (ev.type >= EV_DISC_START) {
            disc_event(&ev);
            continue;
        }
        zlink_t *l = &s_l[ev.idx];
        if (!l->used) {
            continue;
        }
        if (ev.type == EV_SETUP) {
            // Changes made at the device itself (its button, a remote bound to it) come as reports: On/Off at once
            // and at least every 10 min, brightness and colour temperature on a change of 1. A device without the
            // cluster answers "unsupported"; that costs nothing. Not at linking time: with every device of a restart
            // asking at once, requests got lost (2026-11-12: the second lamp had neither binding nor reporting).
            // (a remote also gets this right when it joins, pair.c: later it is asleep)
            if (zb_setup_reports(l->addr, l->ep) != ESP_OK && ++l->su_try < 3) {
                esp_timer_start_once(l->su, 10000000);
            }
            continue;
        }
        if (ev.type == EV_READBACK) {
            // Level before On/Off: the class reports the brightness status as 0 while the light is off, so with
            // the state first it sent the brightness of before for a moment (seen on the bus on 2026-11-12).
            static const uint16_t ct[] = {0x0007}, xy[] = {0x0003, 0x0004, 0x0008};
            l->reads += (zb_read(l->addr, l->ep, 0x0008, a0, 1) == ESP_OK);
            l->reads += (zb_read(l->addr, l->ep, 0x0006, a0, 1) == ESP_OK);
            if (l->ct) {
                l->reads += (zb_read(l->addr, l->ep, 0x0300, ct, 1) == ESP_OK);
            }
            if (l->xy) {
                l->reads += (zb_read(l->addr, l->ep, 0x0300, xy, 3) == ESP_OK);
            }
        } else {
            l->dp_in++;
            l->last_in_us = esp_timer_get_time();
            ESP_LOGI(TAG, "inst %d <- 0x%04x: %s = %s%s", l->inst, l->addr, ev.key, ev.value,
                     ev.initial ? " (initial)" : "");
            bvm_line(l->k,
                     "{\"call\":\"dp_event\",\"inst\":%d,\"id\":%lu,\"ch\":%u,\"key\":\"%s\",\"value\":%s,"
                     "\"initial\":%s}",
                     l->inst, bvm_next_id(), ev.ch, ev.key, ev.value, ev.initial ? "true" : "false");
            if (strcmp(ev.key, "zigbeeModel") == 0) {
                vdev_model_event(l->inst);
            }
        }
    }
}

void zlink_start(void)   // once, before the first link
{
    s_mx = xSemaphoreCreateMutex();
    s_q = xQueueCreate(12, sizeof(lev_t));   // 12, not 8: a colour lamp answers with 7 values at once
    const esp_timer_create_args_t tt = {.callback = tell_fire, .name = "zlink_tell"};
    ESP_ERROR_CHECK(esp_timer_create(&tt, &s_tell));
    xTaskCreate(link_task, "link", 4096, NULL, 4, NULL);
    const esp_timer_create_args_t t = {.callback = tick_fire, .name = "zlink_avail"};
    ESP_ERROR_CHECK(esp_timer_create(&t, &s_tick));
    esp_timer_start_periodic(s_tick, TICK_US);
}

// Ask the device everything once more, as after linking: model, state, binding and reporting, and its class writes
// its parameters into it again (a device reset by hand lost them). What comes back is no initial value (except the
// model): a status that differs from what KNX was told goes on the bus.
bool zlink_reinterview(int inst)
{
    if (!s_mx) {
        return false;
    }
    xSemaphoreTake(s_mx, portMAX_DELAY);
    zlink_t *l = by_inst(inst);
    if (l) {
        l->initial |= INI_MODEL;
        l->reads += (zb_read(l->addr, l->ep, 0x0000, IDENT, 2) == ESP_OK);
        readback(l, 0);
        l->su_try = 0;
        esp_timer_stop(l->su);
        esp_timer_start_once(l->su, 3000000);
        l->params_at = esp_timer_get_time() + PARAMS_US;
        ESP_LOGI(TAG, "inst %d <-> 0x%04x: re-interview (endpoints, model, state, binding and reporting, parameters)",
                 l->inst, l->addr);
    }
    uint16_t addr = l ? l->addr : 0;
    xSemaphoreGive(s_mx);
    if (l) {
        zlink_discover(addr);
    }
    return l != NULL;
}

static void first_reads(zlink_t *l)
{
    // Colour temperature and the range the lamp can do, colour (x, y, mode) and what colour it can do at all: a light
    // without colour control answers "unsupported"
    static const uint16_t a0[] = {0x0000}, ct[] = {0x0007, 0x400B, 0x400C, 0x0003, 0x0004, 0x0008, 0x400A};
    l->initial = INI_ALL;
    zb_read(l->addr, l->ep, 0x0000, IDENT, 2);
    zb_read(l->addr, l->ep, 0x0006, a0, 1);
    zb_read(l->addr, l->ep, 0x0008, a0, 1);
    zb_read(l->addr, l->ep, 0x0300, ct, 7);
    // binding and reporting a little later, one device after the other (EV_SETUP in link_task)
    l->su_try = 0;
    esp_timer_stop(l->su);
    esp_timer_start_once(l->su, 3000000 + (uint64_t)(l - s_l) * 2000000);
}

// A built-in class starts with its default identity and parameters, as an ETS download would bring them. A template
// from a download gets its parameters from that download. The real model follows from the Basic read.
static void defaults(int inst, int k)
{
    if (*bvm_class_model(k)) {
        bvm_line(k, "{\"call\":\"dp_event\",\"inst\":%d,\"id\":%lu,\"ch\":0,\"key\":\"zigbeeModel\",\"value\":\"%s\",\"initial\":true}",
                 inst, bvm_next_id(), bvm_class_model(k));
        bvm_line(k, "{\"call\":\"params\",\"inst\":%d,\"id\":%lu,\"p\":\"%s\"}", inst, bvm_next_id(), bvm_class_seg(k));
    }
}

static int add(int inst, int k, uint16_t addr, uint8_t ep)
{
    if (inst < FIRST_INST || !bvm_slot_ok(k)) {
        return -1;
    }
    if (by_inst(inst) || by_addr(addr)) {
        return -2;
    }
    // The link of this instance to this device was only taken down (zlink_pause) and its device was off then: it is
    // still off, not "not known yet". Otherwise the gateway's fault went to 0 for the seconds until two sends had
    // failed, and "OFF" was reported a second time (2026-11-15, a download with a new template).
    zlink_t *l = NULL, *spare = NULL;
    bool was_off = false;
    for (int i = 0; i < NLINK; i++) {
        zlink_t *x = &s_l[i];
        if (x->used) {
            continue;
        }
        if (x->kept_off && x->inst == inst) {
            was_off = was_off || x->addr == addr;   // another device in its place: nothing is known of that one
            x->kept_off = false;
            l = l ? l : x;
        } else if (!x->kept_off && !l) {
            l = x;
        } else if (!spare) {
            spare = x;   // holds what another instance left: taken only if nothing else is free
        }
    }
    l = l ? l : spare;
    if (!l) {
        return -3;
    }
    l->kept_off = false;
    if (!l->rb) {
        const esp_timer_create_args_t a = {.callback = rb_fire, .arg = (void *)(uintptr_t)(l - s_l), .name = "zlink_rb"};
        ESP_ERROR_CHECK(esp_timer_create(&a, &l->rb));
    }
    if (!l->su) {
        const esp_timer_create_args_t a = {.callback = su_fire, .arg = (void *)(uintptr_t)(l - s_l), .name = "zlink_su"};
        ESP_ERROR_CHECK(esp_timer_create(&a, &l->su));
    }
    l->inst = inst;
    l->k = k;
    l->addr = addr;
    l->ep = ep;
    l->trans = 0;
    l->zcl_out = l->dp_in = l->knx_out = 0;
    l->reads = 3;
    l->last_in_us = 0;
    l->heard_n = l->fail_n = 0;
    l->n_off = l->n_on = 0;
    l->nudged = l->gone = false;
    l->params_at = 0;   // linking hands the class its parameters (vdev.c, apply)
    av_init(&l->av, esp_timer_get_time(), false, 0, 0);   // vdev.c says right after linking whether it sleeps
    if (was_off) {
        l->av.state = AV_OFF;
    }
    l->used = true;
    defaults(inst, k);
    const bool *t = told_of(inst);
    if (was_off && t && !*t) {   // the bus has not heard it since the start: "false" goes there at its turn
        esp_timer_start_once(s_tell, TELL_GAP_US);
    } else if (was_off) {   // the class has an object for it and no value yet: the value as it stands, not a change
        bvm_line(k, "{\"call\":\"dp_event\",\"inst\":%d,\"id\":%lu,\"ch\":0,\"key\":\"availability\",\"value\":false,"
                 "\"initial\":true}", inst, bvm_next_id());
    }
    first_reads(l);
    return 0;
}

int zlink_add(int inst, int k, uint16_t addr, uint8_t ep)
{
    if (!s_mx) {
        return -4;
    }
    xSemaphoreTake(s_mx, portMAX_DELAY);
    int r = add(inst, k, addr, ep);
    xSemaphoreGive(s_mx);
    return r;
}

// Add the link, or move a linked instance to another slot or device (the device server learns the template or the
// class from what ETS downloads). The VM of the old slot forgets the instance.
int zlink_set(int inst, int k, uint16_t addr, uint8_t ep)
{
    if (!s_mx) {
        return -4;
    }
    xSemaphoreTake(s_mx, portMAX_DELAY);
    int r = 0;
    zlink_t *l = by_inst(inst), *other = by_addr(addr);
    if (!l) {
        r = add(inst, k, addr, ep);
    } else if (l->k == k && l->addr == addr && l->ep == ep) {
        r = 0;
    } else if (!bvm_slot_ok(k)) {
        r = -1;
    } else if (other && other != l) {
        r = -2;
    } else {
        bool moved = l->k != k;
        if (moved) {
            bvm_line(l->k, "{\"call\":\"forget\",\"inst\":%d,\"id\":%lu}", inst, bvm_next_id());
        }
        if (l->addr != addr) {   // another short address: what was heard there does not hold
            // An off device that rejoined under a new address is still off until it is heard there: then it comes
            // back as a return (message ON, a value that is sent, binding and reporting anew), not as a first answer
            // It left the network no more: it is here again.
            uint8_t was = l->av.state;
            av_init(&l->av, esp_timer_get_time(), l->av.sleepy, l->heard_n, l->fail_n);
            if (was == AV_OFF) {
                l->av.state = AV_OFF;
            }
            l->gone = false;
        }
        l->k = k;
        l->addr = addr;
        l->ep = ep;
        l->trans = 0;
        l->reads += 3;
        defaults(inst, k);
        // The instance is new to this VM: what is known goes with it. Unless the bus has not heard it since the
        // start: then tell_next() brings it to this VM as a change.
        if (moved && l->av.state != AV_UNKNOWN && !to_tell(l)) {
            bvm_line(k, "{\"call\":\"dp_event\",\"inst\":%d,\"id\":%lu,\"ch\":0,\"key\":\"availability\",\"value\":%s,"
                     "\"initial\":true}", inst, bvm_next_id(), l->av.state == AV_ON ? "true" : "false");
        }
        first_reads(l);
    }
    xSemaphoreGive(s_mx);
    return r;
}

static void take_down(int inst, bool keep)   // keep: it comes back for the same device, remember that it was off
{
    if (!s_mx) {
        return;
    }
    xSemaphoreTake(s_mx, portMAX_DELAY);
    for (int i = 0; i < NLINK; i++) {   // what an earlier link of this instance left
        if (!s_l[i].used && s_l[i].inst == inst) {
            s_l[i].kept_off = s_l[i].kept_off && keep;
        }
    }
    zlink_t *l = by_inst(inst);
    if (l) {
        esp_timer_stop(l->rb);
        esp_timer_stop(l->su);
        l->kept_off = keep && l->av.state == AV_OFF;
        l->params_at = 0;
        l->used = false;   // first: values still on their way find no link any more
        bvm_line(l->k, "{\"call\":\"forget\",\"inst\":%d,\"id\":%lu}", inst, bvm_next_id());
    }
    bool *t = told_of(inst);
    if (t && !keep) {   // the instance goes: whatever device it gets next, the bus has not heard of that one
        *t = false;
    }
    xSemaphoreGive(s_mx);
}

// The device behind an instance is gone: the VM forgets the instance and the Zigbee device is free for another one.
void zlink_del(int inst)
{
    take_down(inst, false);
}

// The instance runs no class for now (vdev.c, halt(): its application changes or was refused) and is linked again to
// the same device when it does. Whether that device was off is kept until then.
void zlink_pause(int inst)
{
    take_down(inst, true);
}

// The device announced itself again at the address it had: its power came back, or it rejoined. Bindings and
// reporting stay with the device, but what it shows now may not be what KNX was told last, and without a change the
// next report is up to 600 s away. Read its state back once it has settled.
void zlink_announced(int inst)
{
    if (!s_mx) {
        return;
    }
    xSemaphoreTake(s_mx, portMAX_DELAY);
    zlink_t *l = by_inst(inst);
    if (l && l->used) {
        readback(l, 13);   // 2 s
        if (l->gone) {     // it had left and is back at the same address
            l->gone = false;
            l->heard_n++;
        }
    }
    xSemaphoreGive(s_mx);
}

void zlink_reread(int inst)   // ask the device for its model again (it did not answer the first time)
{
    zlink_t *l = by_inst(inst);
    if (l) {
        l->initial |= INI_MODEL;
        l->reads += (zb_read(l->addr, l->ep, 0x0000, IDENT, 2) == ESP_OK);
    }
}

esp_err_t zlink_knx_write(int inst, int ko, const char *hex)
{
    zlink_t *l = by_inst(inst);
    if (!l && inst >= FIRST_INST) {
        return ESP_ERR_NOT_FOUND;
    }
    int k = l ? l->k : (inst - 1) % 2;   // simulated instances alternate between the two classes
    return bvm_line(k, "{\"call\":\"knx_write\",\"inst\":%d,\"id\":%lu,\"ko\":%d,\"raw\":\"%s\"}", inst, bvm_next_id(),
                    ko, hex)
               ? ESP_OK
               : ESP_FAIL;
}

static void poll_fire(void *arg)
{
    (void)arg;
    for (int i = 0; i < NLINK && s_q; i++) {
        if (s_l[i].used) {
            lev_t ev = {.type = EV_READBACK, .idx = (uint8_t)i};
            xQueueSend(s_q, &ev, 0);
        }
    }
}

// read every linked device back every `seconds` (0 stops): the light does not change, the link keeps running
void zlink_poll(int seconds)
{
    if (!s_poll) {
        const esp_timer_create_args_t a = {.callback = poll_fire, .name = "zlink_poll"};
        ESP_ERROR_CHECK(esp_timer_create(&a, &s_poll));
    }
    esp_timer_stop(s_poll);
    s_poll_s = seconds > 0 ? seconds : 0;
    if (s_poll_s) {
        esp_timer_start_periodic(s_poll, (uint64_t)s_poll_s * 1000000);
    }
}

void zlink_print(void)
{
    int n = 0;
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < NLINK; i++) {
        zlink_t *l = &s_l[i];
        if (l->used) {
            static const char *const AV[] = {"unknown", "on", "OFF"};
            printf("link: inst %d (class %d) <-> 0x%04x ep %u; zcl out %lu, reads %lu, answers %lu, last answer %lld s "
                   "ago, knx out %lu\n",
                   l->inst, l->k, l->addr, l->ep, (unsigned long)l->zcl_out, (unsigned long)l->reads,
                   (unsigned long)l->dp_in, l->last_in_us ? (long long)((now - l->last_in_us) / 1000000) : -1LL,
                   (unsigned long)l->knx_out);
            printf("      availability %s (%s), frames heard %lu, failed sends %lu (%lu since the last frame), quiet for "
                   "%lld s; went off %lu times, came back %lu times\n",
                   AV[l->av.state % 3], l->av.sleepy ? "sleepy end device" : "receiver on", (unsigned long)l->heard_n,
                   (unsigned long)l->fail_n, (unsigned long)(l->fail_n - l->av.fail_base),
                   (long long)((now - l->av.last_us) / 1000000), (unsigned long)l->n_off, (unsigned long)l->n_on);
            n++;
        }
    }
    if (!n) {
        printf("link: none (zlink <inst 100..> <short address> [ep] [class 0|1])\n");
    }
    if (s_poll_s) {
        printf("link poll: every %d s\n", s_poll_s);
    } else {
        printf("link poll: off\n");
    }
}
