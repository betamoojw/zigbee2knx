// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dirk Tostmann
// Binding a Zigbee device from ETS, without a button on the gateway.
//
// When ETS programs an individual address it first asks the target address (T_Connect, A_DeviceDescriptor_Read) and
// then sends A_IndividualAddress_Read as a broadcast every 3 s until a device in programming mode answers: that is
// its "press the programming button". One such question and the reads of the same source that follow it are a
// series. Seen from here:
//
//   1. The target lies in the range of this gateway and two reads followed: open the Zigbee network once, for three
//      minutes at most. ETS giving up (no read for 7 s) closes it. A second source that asks meanwhile (another ETS)
//      keeps its question as long as its reads go on, and is served when the first series is through.
//   2. The first device that joins and passes the key exchange becomes a virtual KNX device in programming mode and
//      answers the next read. The network closes at once. Whatever else joins in that window is asked to leave.
//      "Joins" is the trust centre's Device_Update "joined", not the device's announce: on 2026-11-15 a lamp joined
//      and finished its key exchange (in 11.9 s) and the stack reported no announce at all.
//   3. ETS writes the address; the device identifies itself, so one sees which one it was. The series ends there,
//      and programming mode ends with it even if ETS never restarts the device.
//   4. A device without an address that is already waiting (ETS gave up before, or a restart) is taken first,
//      but only if it still answers; a silent one is removed and the network opens. A sleepy one (its announce said
//      receiver off when idle: a remote) cannot answer while it sleeps and is taken without the question.
//   5. The target is the address of one of our devices: that device goes into programming mode for the series,
//      which is how ETS programs a device again that has its address already.
//   6. Any opening of the network this firmware did not ask for is closed again, unless a series or the pairing
//      window holds the network open at that moment: then it stays open and ends when they end. Seen after every key
//      exchange recorded since 2026-11-11, 2.4 to 3.7 s after it, for 180 s; whether the device that joined asks for
//      it or the stack does is not established.
//   7. The gateway's own object or button S1 (gw.c) opens a pairing window instead: the first device that joins
//      becomes a virtual device without an address and waits (point 4) until ETS asks for an address in the range.
//   8. A device whose Zigbee device left the network (a reset, a replacement) keeps its address and everything ETS
//      loaded. When ETS asks for that address, the network opens instead of point 5, and the first device that joins
//      takes its place. If it is the device that left, it simply comes back. (2026-11-12: without this a reset lamp
//      stayed outside and ETS loaded the old virtual device.)
//
//   9. ETS unloads the address of a device (it writes 15.15.255): the device is to go. A few seconds later, so that
//      ETS can finish what it does with it, the virtual device is removed and its Zigbee device asked to leave. Until
//      then it is not a device that waits for an address, and a restart in between does not make it one.
//  10. Whoever is asked to leave, because its virtual device went or because it joined where it was not wanted, is
//      remembered by its IEEE address until it is seen leaving. A device that was switched off then is still a member
//      when it comes back: its announce or its rejoin gets it the request again, and so does any frame it sends from
//      the short address it was asked at (a lamp the request did not reach goes on reporting). A new join ends that as
//      well: then it is a device like any other.
//  11. One of our devices that joins anew (Device_Update "joined", not a rejoin) was reset by hand: it stays with its
//      virtual device and is set up again (binding, reporting, its parameters). It does not become the new device
//      an open network waits for.
//
// The read is not addressed to us: every address assignment on the line looks the same. The range is what ties it to
// this gateway; without a target inside it nothing opens. ETS does not say which product it is about to load, so the
// type cannot be matched here; the model is compared with the template's list after the download and only reported
// (vdev.c).
//
// What this does not do: a device that was asked to leave has seen the network key. Keeping it from joining at all
// is a matter of the trust centre (install codes); that is not built.
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "board.h"
#include "app.h"

static const char *TAG = "pair";

#define POLL_GAP_US    7000000     // ETS reads every 3 s; one may get lost
// From the question to the target address to the first read. ETS 6.4.1 sends the read 21 to 27 ms after its T_Connect
// (recordings of 2026-11-10 and 2026-11-12). Kept short on purpose: a T_Connect to one of our devices for any
// other reason must not turn the address reads of an assignment elsewhere into a series for that device.
#define PROBE_VALID_US 2000000
// From the join to the end of the key exchange. The stack reports a failed or timed-out exchange itself (authorized
// with a status); this is the backstop behind it. 10 s were too few: see point 2.
#define AUTH_WAIT_US   30000000
#define AGAIN_US       10000000    // the same device is not asked to leave twice within this
#define GUARD_US       15000000    // after closing: joins that were already under way are asked to leave
// A series opens the network once, for as long as a series may keep it open, and closes it when a device joins or ETS
// stops asking. Not in short openings renewed with every read: on 2026-11-12 (evening) a lamp and a plug that were
// searching did not join in three minutes of openings of 10 s renewed every 6 s (0.5.0 and 0.6.0), while the plug
// joined 47 s after one opening of 180 s. Why is not established; the renewal is the difference.
#define OPEN_S         180
#define OPEN_MAX_US    180000000   // one series keeps the network open no longer than this
#define DONE_US        10000000    // after the address is written, programming mode ends at the latest then
#define CHECK_US       3000000     // a waiting device has this long to answer before it is offered to ETS
// A device that joined here and got no address is removed after this. Long enough to pair a device through the window
// (object or button of the gateway) and walk over to ETS afterwards.
#define WAIT_MAX_US    1800000000
#define WINDOW_MAX_S   254         // the longest permit-join Zigbee knows short of "for ever"
// From ETS unloading an address to the removal: ETS reads the address back by serial number after it wrote it
// (2026-11-15), and the device answers that from 15.15.255.
#define UNLOAD_US      5000000
#define SCAN_US        10000000    // how often devices without an address are looked over
#define PEND_MAX       KNX_VDEV_MAX   // devices asked to leave that have not been seen leaving (point 10)
#define IDENTIFY_S     5
// the reason of the message "<address> DEL"; gw.c keeps 79 characters
#define REMOVED_WHY    "ETS unloaded the address: virtual device removed, Zigbee device asked to leave"

// ST_WINDOW: the gateway's object or button opened the network (gw.c); the first device that joins waits for ETS
enum { ST_IDLE, ST_OPEN, ST_AUTH, ST_CHECK, ST_PENDING, ST_DONE, ST_WINDOW };
enum { EV_PROBE, EV_POLL, EV_ASSIGNED, EV_UPDATE, EV_ANNOUNCE, EV_AUTHORIZED, EV_LEFT, EV_OPENED, EV_HEARD, EV_WINDOW,
       EV_UNLOADED, EV_REMOVED, EV_PROG_ENDED };

typedef struct {
    uint8_t type, ok;
    uint16_t a, b;    // probe: source, target; poll: source; assigned: slot, address; Zigbee: short address;
                      // unloaded: slot, the address it had
    int8_t own;       // probe: the virtual device that holds the target address, -1: nobody here
    uint8_t cap;      // announce: MAC capability flags (bit 3: receiver on when idle)
    uint8_t ieee[8];  // update, announce, left, removed
} ev_t;

static QueueHandle_t s_q;
static nvs_handle_t s_nvs;
static bool s_nvs_ok, s_enabled = true;
static uint16_t s_lo = 0xFF01, s_hi = 0xFF30;   // default 15.15.1 .. 15.15.48; gw.c or the console sets another range
static atomic_uint s_dropped;

// The last question of every source that asks (an ETS, through whichever interface): the address it asked for, and
// whether its reads follow it. A question counts when its first read comes within PROBE_VALID_US, and from then on as
// long as its source keeps reading with gaps of POLL_GAP_US at most. While one source's series runs, another one's
// question stays good this way, and that source is served when the first is through. A question starts one series.
#define SRC_MAX 4
typedef struct {
    uint16_t src, dst;
    int8_t own;          // our device at dst, -1: none
    bool used, live;     // live: its reads follow it
    bool spent;          // it started a series, or it was found to be none of ours
    int64_t t, last;     // when it was asked; its last read, 0: none yet
} question_t;
static question_t s_qs[SRC_MAX];

// the series that grew out of a question
static uint16_t s_src, s_target;
static int s_own = -1;
static int64_t s_poll_last, s_note_t;
static int s_polls;
static bool s_armed, s_told;

static volatile int s_st = ST_IDLE;
static int64_t s_opened, s_closed, s_cand_t, s_check_t, s_done_t, s_window_end;
static uint16_t s_cand, s_check_sa;
static uint8_t s_cand_ieee[8], s_cand_cap;
static bool s_have_cand, s_heard, s_cand_window;   // s_cand_window: the candidate came through the window
static bool s_cand_cap_known;                      // its announce came: its MAC capability flags are known
static int s_slot = -1;                          // the virtual device in programming mode for this series
static int s_replace = -1;                       // the series opened the network for a device whose Zigbee device
                                                 // left: the first that joins takes its place
static int64_t s_born[KNX_VDEV_MAX];             // when a device was made here that still has no address, 0: none
static int64_t s_unload[KNX_VDEV_MAX];           // when ETS unloaded its address, 0: it did not
static uint16_t s_unload_ia[KNX_VDEV_MAX];       // ... and the address it had
// The MAC capability its device announced, for the device a place was made for (point 4: a sleepy one cannot be
// asked whether it is still there). Valid while the place holds that IEEE address.
static bool s_cap_known[KNX_VDEV_MAX], s_cap_rx_on[KNX_VDEV_MAX];
static uint8_t s_cap_ieee[KNX_VDEV_MAX][8];
#define SA_NONE 0xFFFF                           // no short address known
static uint8_t s_pend[PEND_MAX][8];              // asked to leave, no leave indication yet; the oldest first
static volatile uint16_t s_pend_sa[PEND_MAX];    // ... the short address it had when it was asked or seen last
static int64_t s_pend_t[PEND_MAX];               // ... when it was asked last (not kept over a restart)
static volatile uint8_t s_pend_n;
static uint32_t s_n_open, s_n_bound, s_n_asked, s_n_ignored, s_n_reclosed, s_n_expired, s_n_window, s_n_waiting;
static uint32_t s_n_unloaded, s_n_again;
static int64_t s_scan_t;

static void ia_str(uint16_t a, char out[12])
{
    snprintf(out, 12, "%u.%u.%u", a >> 12, (a >> 8) & 0x0F, a & 0xFF);
}

static void post(const ev_t *e)   // from the device server and the Zigbee task: never block
{
    if (!s_q || xQueueSend(s_q, e, 0) != pdTRUE) {
        atomic_fetch_add(&s_dropped, 1);
    }
}

void pair_knx_probe(uint16_t src, uint16_t target, int own_slot)
{
    ev_t e = {.type = EV_PROBE, .a = src, .b = target, .own = (int8_t)own_slot};
    post(&e);
}

void pair_knx_poll(uint16_t src)
{
    ev_t e = {.type = EV_POLL, .a = src};
    post(&e);
}

void pair_knx_assigned(int slot, uint16_t ia)
{
    ev_t e = {.type = EV_ASSIGNED, .a = (uint16_t)slot, .b = ia};
    post(&e);
}

void pair_zb_update(uint16_t short_addr, const uint8_t ieee[8], int status)
{
    ev_t e = {.type = EV_UPDATE, .a = short_addr, .b = (uint16_t)status};
    memcpy(e.ieee, ieee, 8);
    post(&e);
}

void pair_zb_announce(uint16_t short_addr, const uint8_t ieee[8], uint8_t capability)
{
    ev_t e = {.type = EV_ANNOUNCE, .a = short_addr, .cap = capability};
    memcpy(e.ieee, ieee, 8);
    post(&e);
}

void pair_zb_authorized(uint16_t short_addr, bool ok)
{
    ev_t e = {.type = EV_AUTHORIZED, .a = short_addr, .ok = ok};
    post(&e);
}

void pair_zb_left(uint16_t short_addr, const uint8_t ieee[8])
{
    ev_t e = {.type = EV_LEFT, .a = short_addr};
    memcpy(e.ieee, ieee, 8);
    post(&e);
}

void pair_knx_unloaded(int slot, uint16_t old_ia)
{
    ev_t e = {.type = EV_UNLOADED, .a = (uint16_t)slot, .b = old_ia};
    post(&e);
}

void pair_knx_prog_ended(int slot)   // the device server ended its programming mode after 4 min (vdev.c, PROG_US)
{
    ev_t e = {.type = EV_PROG_ENDED, .a = (uint16_t)slot};
    post(&e);
}

void pair_removed(const uint8_t ieee[8])
{
    ev_t e = {.type = EV_REMOVED};
    memcpy(e.ieee, ieee, 8);
    post(&e);
}

void pair_zb_opened(uint8_t seconds)   // the network opened without this firmware asking for it
{
    ev_t e = {.type = EV_OPENED, .a = seconds};
    post(&e);
}

void pair_window(int seconds)   // from gw.c (object or button): never blocks
{
    ev_t e = {.type = EV_WINDOW, .a = (uint16_t)(seconds < 0 ? 0 : seconds)};
    post(&e);
}

bool pair_is_open(void)
{
    int st = s_st;
    return st == ST_OPEN || st == ST_WINDOW;
}

void pair_zb_heard(uint16_t short_addr)   // a ZCL frame came from that device (in the Zigbee task)
{
    // Only a waiting device that is being asked, or a member that was asked to leave, matters here; the rest of the
    // time this saves the queue. The list is read without a lock: a frame missed while it changes comes again.
    bool asked = false;
    for (int i = 0, n = s_pend_n; i < n && i < PEND_MAX && !asked; i++) {
        asked = s_pend_sa[i] == short_addr;
    }
    if (s_st == ST_CHECK || asked) {
        ev_t e = {.type = EV_HEARD, .a = short_addr};
        post(&e);
    }
}

static void close_network(int64_t now, const char *why)
{
    esp_err_t e = zb_permit_join(0);
    s_closed = now;
    ESP_LOGI(TAG, "Zigbee network closed (%s): %s", why, esp_err_to_name(e));
}

static void pend_add(const uint8_t ieee[8], uint16_t sa, int64_t asked);

// With its IEEE address the device is remembered until it is seen leaving: a request that does not reach it (the lamp
// lost power right after joining) is repeated when it shows up again.
static void ask_to_leave(uint16_t short_addr, const uint8_t *ieee, const char *why)
{
    esp_err_t e = zb_leave(short_addr, ieee);
    s_n_asked++;
    ESP_LOGW(TAG, "0x%04x %s: asked to leave the network (%s)", short_addr, why, esp_err_to_name(e));
    if (ieee) {
        pend_add(ieee, short_addr, esp_timer_get_time());
    }
}

// ---------------------------------------------------------------- asked to leave, not seen leaving (point 10)
// The IEEE addresses ("leave") and, in the same order, the short addresses ("leave_sa") are kept in the flash: after a
// restart a member that sends reports is known by its short address as well.
static int pend_find(const uint8_t ieee[8])
{
    for (int i = 0; i < s_pend_n; i++) {
        if (memcmp(s_pend[i], ieee, 8) == 0) {
            return i;
        }
    }
    return -1;
}

static int pend_by_sa(uint16_t sa)
{
    for (int i = 0; i < s_pend_n && sa != SA_NONE; i++) {
        if (s_pend_sa[i] == sa) {
            return i;
        }
    }
    return -1;
}

static void pend_save(void)
{
    if (!s_nvs_ok) {
        return;
    }
    uint16_t sa[PEND_MAX];
    for (int i = 0; i < s_pend_n; i++) {
        sa[i] = s_pend_sa[i];
    }
    esp_err_t e = s_pend_n ? nvs_set_blob(s_nvs, "leave", s_pend, (size_t)s_pend_n * 8) : nvs_erase_key(s_nvs, "leave");
    if (e == ESP_OK || e == ESP_ERR_NVS_NOT_FOUND) {
        e = s_pend_n ? nvs_set_blob(s_nvs, "leave_sa", sa, (size_t)s_pend_n * 2) : nvs_erase_key(s_nvs, "leave_sa");
    }
    if (e == ESP_OK || e == ESP_ERR_NVS_NOT_FOUND) {
        e = nvs_commit(s_nvs);
    }
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "the list of devices asked to leave is not saved (%s)", esp_err_to_name(e));
    }
}

static void pend_load(void)
{
    size_t n = sizeof s_pend, m;
    uint16_t sa[PEND_MAX];
    s_pend_n = 0;
    if (s_nvs_ok && nvs_get_blob(s_nvs, "leave", s_pend, &n) == ESP_OK && n % 8 == 0) {
        s_pend_n = (uint8_t)(n / 8);
    }
    m = sizeof sa;
    bool have_sa = s_nvs_ok && nvs_get_blob(s_nvs, "leave_sa", sa, &m) == ESP_OK && m == (size_t)s_pend_n * 2;
    for (int i = 0; i < PEND_MAX; i++) {   // a list saved by older firmware has no short addresses
        s_pend_sa[i] = have_sa && i < s_pend_n ? sa[i] : SA_NONE;
        s_pend_t[i] = 0;
    }
}

static void pend_remove(int i)
{
    for (int j = i; j < s_pend_n - 1; j++) {
        memcpy(s_pend[j], s_pend[j + 1], 8);
        s_pend_sa[j] = s_pend_sa[j + 1];
        s_pend_t[j] = s_pend_t[j + 1];
    }
    s_pend_n--;
}

static bool pend_drop(const uint8_t ieee[8])
{
    int i = pend_find(ieee);
    if (i < 0) {
        return false;
    }
    pend_remove(i);
    pend_save();
    return true;
}

// asked: when the request went out, 0: not known (it is asked when it shows up)
static void pend_add(const uint8_t ieee[8], uint16_t sa, int64_t asked)
{
    static const uint8_t none[8];
    if (memcmp(ieee, none, 8) == 0) {
        return;
    }
    int i = pend_find(ieee);
    if (i >= 0) {   // remembered already: asked once more, perhaps at another short address
        s_pend_t[i] = asked ? asked : s_pend_t[i];
        if (sa != SA_NONE && sa != s_pend_sa[i]) {
            s_pend_sa[i] = sa;
            pend_save();
        }
        return;
    }
    if (s_pend_n == PEND_MAX) {   // the oldest goes: it stays a member nobody serves
        pend_remove(0);
    }
    memcpy(s_pend[s_pend_n], ieee, 8);
    s_pend_sa[s_pend_n] = sa;
    s_pend_t[s_pend_n] = asked;
    s_pend_n++;
    pend_save();
}

// The device with this IEEE address uses this short address now (its announce, its Device_Update). A remembered
// device follows it; one remembered under that short address that is another device does not have it any more.
static void pend_seen(uint16_t sa, const uint8_t ieee[8])
{
    bool changed = false;
    for (int i = 0; i < s_pend_n; i++) {
        uint16_t now_sa = memcmp(s_pend[i], ieee, 8) == 0 ? sa : s_pend_sa[i] == sa ? SA_NONE : s_pend_sa[i];
        if (now_sa != s_pend_sa[i]) {
            s_pend_sa[i] = now_sa;
            changed = true;
        }
    }
    if (changed) {
        pend_save();
    }
}

// ---------------------------------------------------------------- unloaded and not removed yet (point 9), in the flash
// Without this a restart in the seconds between the unloading and the removal left a device without an address that
// waited for 30 min like one that had just joined, and the next assignment would have taken it.
static void unl_save(void)
{
    if (!s_nvs_ok) {
        return;
    }
    uint16_t ia[KNX_VDEV_MAX];
    bool any = false;
    for (int i = 0; i < KNX_VDEV_MAX; i++) {
        ia[i] = s_unload[i] ? s_unload_ia[i] : 0;
        any = any || ia[i];
    }
    esp_err_t e = any ? nvs_set_blob(s_nvs, "unload", ia, sizeof ia) : nvs_erase_key(s_nvs, "unload");
    if (e == ESP_OK) {
        e = nvs_commit(s_nvs);
    }
    if (e != ESP_OK && e != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "the devices ETS unloaded are not saved (%s)", esp_err_to_name(e));
    }
}

static void unl_clear(int i)
{
    if (s_unload[i]) {
        s_unload[i] = 0;
        unl_save();
    }
}

static void unl_load(void)   // at the start: who was unloaded and still has no address goes with the first tick
{
    uint16_t ia[KNX_VDEV_MAX];
    size_t n = sizeof ia;
    if (!s_nvs_ok || nvs_get_blob(s_nvs, "unload", ia, &n) != ESP_OK || n != sizeof ia) {
        return;
    }
    bool changed = false;
    for (int i = 0; i < KNX_VDEV_MAX; i++) {
        if (ia[i] && vdev_unprogrammed(i)) {
            s_unload[i] = 1;   // long ago
            s_unload_ia[i] = ia[i];
        } else if (ia[i]) {
            changed = true;    // it has an address again or is gone
        }
    }
    if (changed) {
        unl_save();
    }
}

// ---------------------------------------------------------------- what a device announced about its receiver
static void cap_note(int i, const uint8_t ieee[8], bool known, uint8_t capability)
{
    if (i < 0 || i >= KNX_VDEV_MAX) {
        return;
    }
    s_cap_known[i] = known;
    s_cap_rx_on[i] = capability & 0x08;   // MAC capability flags, bit 3: receiver on when idle
    memcpy(s_cap_ieee[i], ieee, 8);
}

// The Zigbee device of virtual device i is a sleepy end device: its receiver is off between its polls, so a read
// reaches it only when it wakes, which may be minutes later (a remote). Known from its announce.
static bool sleeps(int i)
{
    uint8_t ieee[8];
    if (i < 0 || i >= KNX_VDEV_MAX) {
        return false;
    }
    if (s_cap_known[i] && vdev_ieee(i, ieee) && memcmp(ieee, s_cap_ieee[i], 8) == 0) {
        return !s_cap_rx_on[i];
    }
    return vdev_rx_on(i) == 0;   // not seen since a restart: what the device record kept from an earlier announce
}

// A virtual device goes for good, and its Zigbee device with it. The series that had it in hand is over.
static void remove_device(int i, const char *why)
{
    uint16_t za;
    uint8_t ep, ieee[8];
    if (i < 0 || i >= KNX_VDEV_MAX) {
        return;
    }
    if (vdev_zigbee(i, &za, &ep) && !vdev_is_gone(i)) {
        ask_to_leave(za, NULL, why);   // without the address in the request, as shown on the bench
        if (vdev_ieee(i, ieee)) {
            pend_add(ieee, za, esp_timer_get_time());
        }
    }
    vdev_del(i);
    s_born[i] = 0;
    s_cap_known[i] = false;
    unl_clear(i);
    if (i == s_slot) {
        s_slot = -1;
        if (s_st == ST_PENDING || s_st == ST_DONE) {   // it was in programming mode for ETS: that series is over
            s_st = ST_IDLE;
            s_armed = false;
        } else if (s_st == ST_CHECK) {   // it was being asked whether it is there: the next read opens the network
            s_st = ST_IDLE;
        }
    }
    if (i == s_replace) {   // the network is open for a new device in its place: there is no place any more
        s_replace = -1;
        if (s_st == ST_OPEN) {
            close_network(esp_timer_get_time(), why);
            s_st = ST_IDLE;
            s_armed = false;
        }
    }
}

static void end_series(int64_t now, const char *why)   // whatever the series had under way is put back
{
    if (s_st == ST_WINDOW || (s_st == ST_AUTH && s_cand_window)) {   // the window belongs to no series: it runs on
        s_slot = -1;
        s_armed = false;
        return;
    }
    if (s_st == ST_OPEN) {
        close_network(now, why);
    } else if ((s_st == ST_PENDING || s_st == ST_DONE) && s_slot >= 0) {
        vdev_prog(s_slot, false);
        ESP_LOGI(TAG, "%s: virtual device %d leaves programming mode", why, s_slot);
    } else if (s_st == ST_AUTH && s_have_cand) {
        ask_to_leave(s_cand, s_cand_ieee, why);
    }
    s_slot = -1;
    s_replace = -1;
    s_have_cand = false;
    s_st = ST_IDLE;
    s_armed = false;
}

static void candidate_ok(int64_t now)
{
    // Binding and reporting now, while the device is awake from joining: a remote sleeps by the time ETS has loaded
    // its virtual device, and only the binding sends its button commands here.
    zb_setup_reports(s_cand, 1);
    if (s_replace >= 0) {   // it takes the place of a device that left: address, tables, parameters stay
        int r = s_replace;
        char t[12];
        ia_str(s_target, t);
        s_replace = -1;
        s_have_cand = false;
        if (!vdev_replace(r, s_cand, s_cand_ieee)) {
            ask_to_leave(s_cand, s_cand_ieee, "found its virtual device gone");
            s_st = ST_IDLE;
            return;
        }
        if (s_cand_cap_known) {   // otherwise the node descriptor of the interview brings it
            vdev_set_capability(s_cand_ieee, s_cand_cap);
        }
        cap_note(r, s_cand_ieee, s_cand_cap_known, s_cand_cap);
        zlink_discover(s_cand);   // the full interview while it is awake: which endpoint has its function
        vdev_prog(r, true);
        s_slot = r;
        s_st = ST_PENDING;
        s_n_bound++;
        esp_err_t e = zb_identify(s_cand, 1, IDENTIFY_S);
        ESP_LOGI(TAG, "0x%04x took the place of the device that left: virtual device %d (%s), in programming mode for "
                 "ETS; it identifies itself for %d s (%s)", s_cand, r, t, IDENTIFY_S, esp_err_to_name(e));
        return;
    }
    // No class and nothing written into the device before ETS has loaded it: what it is comes with the download.
    int n = vdev_add(s_cand, 1, -1, s_cand_ieee);
    s_have_cand = false;
    if (n < 0) {
        ask_to_leave(s_cand, s_cand_ieee, "found no free virtual device");
        gw_note(GW_NOTE_FULL, s_cand_window ? 0xFFFF : s_target, 0,
                "a Zigbee device joined, but no virtual device is free: asked to leave");
        s_st = ST_IDLE;
        return;
    }
    if (s_cand_cap_known) {
        vdev_set_capability(s_cand_ieee, s_cand_cap);
    }
    cap_note(n, s_cand_ieee, s_cand_cap_known, s_cand_cap);
    zlink_discover(s_cand);   // the full interview while it is awake: which endpoint has its function
    s_born[n] = now;
    if (s_cand_window) {   // nobody asks for an address yet: it waits, and the next question in the range takes it
        s_n_waiting++;
        s_st = ST_IDLE;
        ESP_LOGI(TAG, "0x%04x is virtual device %d now and waits for ETS to give it an address (removed if none comes "
                 "within %d min)", s_cand, n, (int)(WAIT_MAX_US / 60000000));
        return;
    }
    vdev_prog(n, true);
    s_slot = n;
    s_st = ST_PENDING;
    char t[12];
    ia_str(s_target, t);
    ESP_LOGI(TAG, "0x%04x is virtual device %d now, in programming mode for %s", s_cand, n, t);
}

static void on_window(int seconds, int64_t now)
{
    if (seconds == 0) {
        if (s_st == ST_WINDOW) {
            s_st = ST_IDLE;
            close_network(now, "pairing stopped");
        }
        return;
    }
    if (s_st == ST_WINDOW) {   // asked again while open: from now on, as long again
        seconds = seconds > WINDOW_MAX_S ? WINDOW_MAX_S : seconds;
        if (zb_permit_join(seconds) == ESP_OK) {
            s_window_end = now + (int64_t)seconds * 1000000;
        }
        return;
    }
    if (s_st != ST_IDLE) {
        ESP_LOGW(TAG, "pairing window not opened: a binding from ETS is under way");
        return;
    }
    if (vdev_free_slots() == 0) {
        ESP_LOGW(TAG, "pairing window not opened: all %d virtual devices are in use", KNX_VDEV_MAX);
        gw_note(GW_NOTE_FULL, 0xFFFF, 0, "pairing window not opened: all virtual devices are in use");
        return;
    }
    seconds = seconds > WINDOW_MAX_S ? WINDOW_MAX_S : seconds;
    esp_err_t e = zb_permit_join(seconds);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "pairing window: the Zigbee network does not open (%s)", esp_err_to_name(e));
        return;
    }
    s_opened = now;
    s_window_end = now + (int64_t)seconds * 1000000;
    s_have_cand = false;
    s_st = ST_WINDOW;
    s_n_window++;
    ESP_LOGI(TAG, "pairing window open for %d s: the first device that joins waits for ETS to give it an address",
             seconds);
}

static void begin(int64_t now)   // two reads of an armed series and nothing under way yet
{
    char t[12];
    ia_str(s_target, t);
    if (s_own >= 0 && vdev_is_gone(s_own)) {   // its Zigbee device left: a new one takes its place
        esp_err_t e = zb_permit_join(OPEN_S);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "ETS asks for %s: the Zigbee network does not open (%s)", t, esp_err_to_name(e));
            return;
        }
        s_replace = s_own;
        s_opened = now;
        s_have_cand = false;
        s_st = ST_OPEN;
        s_n_open++;
        ESP_LOGI(TAG, "ETS asks for %s, the address of virtual device %d, whose Zigbee device left the network: Zigbee "
                 "network open, the first device that joins takes its place", t, s_own);
        return;
    }
    if (s_own >= 0) {
        vdev_prog(s_own, true);
        s_slot = s_own;
        s_st = ST_PENDING;
        ESP_LOGI(TAG, "ETS asks for %s, the address of virtual device %d: it goes into programming mode", t, s_own);
        return;
    }
    if (vdev_any_prog()) {
        if (!s_told) {
            s_told = true;
            ESP_LOGI(TAG, "ETS asks for %s: a device is in programming mode already and answers, nothing opens", t);
        }
        return;
    }
    int w = -1;
    for (int i = 0; i < KNX_VDEV_MAX && w < 0; i++) {   // a device ETS unloaded is on its way out, it does not wait
        if (!s_unload[i] && vdev_unbound(i)) {
            w = i;
        }
    }
    uint16_t za;
    uint8_t ep;
    if (w >= 0 && vdev_zigbee(w, &za, &ep) && sleeps(w)) {
        // A sleepy device (a remote that came through the pairing window) hears a question only when it wakes: it
        // would be removed for not answering. It is offered as it is.
        vdev_prog(w, true);
        s_slot = w;
        s_st = ST_PENDING;
        ESP_LOGI(TAG, "ETS asks for %s: virtual device %d waits without an address; 0x%04x sleeps and cannot be asked "
                 "whether it is still there: it goes into programming mode as it is", t, w, za);
        return;
    }
    if (w >= 0 && vdev_zigbee(w, &za, &ep)) {   // somebody waits without an address: is it still there?
        static const uint16_t model[] = {0x0005};
        s_slot = w;
        s_check_sa = za;
        s_heard = false;
        s_check_t = now;
        s_st = ST_CHECK;
        esp_err_t e = zb_read(za, ep, 0x0000, model, 1);
        ESP_LOGI(TAG, "ETS asks for %s: virtual device %d waits without an address, asking 0x%04x whether it is still "
                 "there (%s)", t, w, za, esp_err_to_name(e));
        return;
    }
    if (vdev_free_slots() == 0) {
        if (!s_told) {
            s_told = true;
            ESP_LOGW(TAG, "ETS asks for %s: all %d virtual devices are in use, nothing opens", t, KNX_VDEV_MAX);
            gw_note(GW_NOTE_FULL, s_target, 0, "ETS asks for an address, but all virtual devices are in use");
        }
        return;
    }
    esp_err_t e = zb_permit_join(OPEN_S);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "ETS asks for %s: the Zigbee network does not open (%s)", t, esp_err_to_name(e));
        return;
    }
    s_opened = now;
    s_have_cand = false;
    s_st = ST_OPEN;
    s_n_open++;
    ESP_LOGI(TAG, "ETS asks for %s: Zigbee network open, the first device that joins gets the address", t);
}

static question_t *question_of(uint16_t src, bool make)
{
    question_t *q = NULL;
    for (int i = 0; i < SRC_MAX; i++) {
        if (s_qs[i].used && s_qs[i].src == src) {
            return &s_qs[i];
        }
    }
    if (!make) {
        return NULL;
    }
    for (int i = 0; i < SRC_MAX; i++) {   // a free entry, else the one that was heard of longest ago
        question_t *c = &s_qs[i];
        if (!c->used) {
            q = c;
            break;
        }
        if (!q || (c->last > c->t ? c->last : c->t) < (q->last > q->t ? q->last : q->t)) {
            q = c;
        }
    }
    *q = (question_t){.src = src, .used = true};
    return q;
}

static void on_probe(const ev_t *e, int64_t now)
{
    if (s_st == ST_DONE && e->a == s_src && e->b != s_target) {
        // The same ETS asks for another address before the 10 s after the last one are over: that assignment is
        // finished. Without this the reads of the new one were dropped as "once more after the write", and when the
        // 10 s were over its question was too old to count: nothing opened.
        vdev_prog(s_slot, false);
        s_slot = -1;
        s_st = ST_IDLE;
    }
    if (s_armed && e->a == s_src) {   // the same ETS connects again: what ran for the address before is over
        // It connects to the address it asked for when the device in programming mode answered from that very address
        // (nothing to write then); anything else is a new question.
        end_series(now, e->b == s_target && s_st == ST_PENDING ? "ETS found the device at its address"
                                                              : "ETS asks for another address");
    }
    question_t *q = question_of(e->a, true);
    q->dst = e->b;
    q->own = e->own;
    q->t = now;
    q->last = 0;
    q->live = q->spent = false;
}

static void on_poll(uint16_t src, int64_t now)
{
    if (s_st == ST_DONE && src == s_src) {
        return;   // ETS reads once more after it wrote the address; the device answers, nothing else to do
    }
    question_t *q = question_of(src, false);
    if (q && !q->spent) {
        if (q->live && now - q->last > POLL_GAP_US) {
            q->live = false;   // its reads stopped for longer than ETS waits between two: that question is over
        } else if (!q->live && !q->last && now - q->t < PROBE_VALID_US) {
            q->live = true;    // the first read follows the question
        }
        q->last = q->live ? now : q->last ? q->last : now;
    }
    if (!s_armed) {
        if (!q || !q->live || q->spent) {   // reads without a question we saw, or of somebody else: not ours to act on
            if (now - s_note_t > 30000000) {
                s_note_t = now;
                s_n_ignored++;
                ESP_LOGI(TAG, "address reads on the line that follow no question to an address of this gateway: "
                         "nothing opens");
            }
            return;
        }
        q->spent = true;   // a question starts one series
        s_src = src;
        s_target = q->dst;
        s_own = q->own;
        s_polls = 0;
        s_told = false;
        if (!s_enabled || (s_own < 0 && (s_target < s_lo || s_target > s_hi))) {
            char t[12];
            ia_str(s_target, t);
            s_n_ignored++;
            s_note_t = now;
            ESP_LOGI(TAG, "ETS asks for %s: %s, nothing opens", t,
                     s_enabled ? "not an address of this gateway" : "binding from ETS is switched off");
            return;
        }
        s_armed = true;
    } else if (src != s_src) {
        return;   // somebody else reads as well: the series belongs to the one who asked
    }
    s_polls++;
    s_poll_last = now;
    if (s_polls < 2) {
        return;
    }
    if (s_st == ST_IDLE) {
        begin(now);   // the network stays open from here: no renewal (see OPEN_S)
    }
}

static void on_tick(int64_t now)
{
    bool asking = s_armed && now - s_poll_last <= POLL_GAP_US;
    switch (s_st) {
    case ST_OPEN:
        if (!asking) {
            end_series(now, "ETS stopped asking");
        } else if (now - s_opened > OPEN_MAX_US) {
            ESP_LOGW(TAG, "open for %d s and no device joined: start the address assignment again",
                     OPEN_MAX_US / 1000000);
            end_series(now, "long enough");
        }
        break;
    case ST_AUTH:
        if (now - s_cand_t > AUTH_WAIT_US) {   // the stack said nothing either way
            ask_to_leave(s_cand, s_cand_ieee, "did not finish the key exchange");
            s_have_cand = false;
            s_st = ST_IDLE;   // the next read opens again
        }
        break;
    case ST_CHECK:
        if (s_heard) {
            vdev_prog(s_slot, true);
            s_st = ST_PENDING;
            ESP_LOGI(TAG, "0x%04x answers: virtual device %d goes into programming mode", s_check_sa, s_slot);
        } else if (now - s_check_t > CHECK_US) {
            ESP_LOGW(TAG, "0x%04x does not answer: virtual device %d is removed, it would have taken the address of "
                     "another device", s_check_sa, s_slot);
            remove_device(s_slot, "waited without an address and does not answer");
            s_slot = -1;
            s_st = ST_IDLE;   // the next read opens the network
        }
        break;
    case ST_PENDING:
        if (!asking) {
            end_series(now, "ETS stopped asking");
        }
        break;
    case ST_DONE:
        if (now - s_done_t > DONE_US) {   // ETS restarts the device within a second; if it did not, this does
            vdev_prog(s_slot, false);
            s_slot = -1;
            s_st = ST_IDLE;
        }
        break;
    case ST_WINDOW:
        if (now > s_window_end) {   // the stack closes it by itself; said here as well, for the log and the status
            s_st = ST_IDLE;
            close_network(now, "pairing window over, nobody joined");
        }
        if (s_armed && !asking) {
            s_armed = false;
        }
        break;
    default:
        if (s_armed && !asking) {
            s_armed = false;
        }
        break;
    }
    bool scan = now - s_scan_t >= SCAN_US;
    if (scan) {
        s_scan_t = now;
    }
    for (int i = 0; i < KNX_VDEV_MAX; i++) {   // devices that joined here and never got an address
        if (i == s_slot) {
            continue;
        }
        if (!s_born[i]) {   // nobody told (an event lost, a device made at the console): its time starts now
            if (scan && !s_unload[i] && vdev_unbound(i)) {
                s_born[i] = now;
            }
            continue;
        }
        if (!vdev_unbound(i)) {
            s_born[i] = 0;
        } else if (now - s_born[i] > WAIT_MAX_US) {
            s_n_expired++;
            remove_device(i, "waited too long without an address");
            ESP_LOGW(TAG, "virtual device %d waited %d min without an address: removed, its device asked to leave", i,
                     WAIT_MAX_US / 60000000);
        }
    }
    for (int i = 0; i < KNX_VDEV_MAX; i++) {   // devices whose address ETS unloaded (point 9)
        if (!s_unload[i] || now - s_unload[i] <= UNLOAD_US) {
            continue;
        }
        uint16_t old = s_unload_ia[i];
        if (!vdev_unprogrammed(i)) {
            unl_clear(i);
            continue;   // it got an address again, or it is gone already
        }
        char t[12];
        ia_str(old, t);
        s_n_unloaded++;
        remove_device(i, "ETS unloaded its address");
        gw_note(GW_NOTE_REMOVED, old, 0, REMOVED_WHY);
        ESP_LOGW(TAG, "virtual device %d (%s until ETS unloaded it) is removed", i, t);
    }
}

static void on_left(uint16_t sa, const uint8_t ieee[8]);

// A member that was asked to leave shows up again (its announce after a power cut, a rejoin, a report it sends): the
// request did not reach it then, it gets it again. True: it was asked.
static bool still_here(uint16_t sa, const uint8_t ieee[8], int64_t now)
{
    int i = pend_find(ieee);
    if (i < 0) {
        return false;
    }
    if (vdev_by_ieee(ieee) >= 0) {
        pend_drop(ieee);   // it has a virtual device again: nothing is left to ask (a new join drops it in on_update)
        return false;
    }
    if (s_pend_t[i] && now - s_pend_t[i] < AGAIN_US) {
        return true;   // asked a moment ago: its rejoin, its announce and its reports come together
    }
    s_n_again++;
    s_pend_t[i] = now;
    ask_to_leave(sa, NULL, "was asked to leave and is still a member");
    return true;
}

// A frame came from the short address a member that was asked to leave had (pair_zb_heard). That address may be one
// of our devices' by now: then it is not the member any more, and nobody is asked.
static void pend_heard(uint16_t sa, int64_t now)
{
    int i = pend_by_sa(sa);
    if (i < 0) {
        return;
    }
    uint8_t ieee[8];
    memcpy(ieee, s_pend[i], 8);
    if (vdev_by_ieee(ieee) < 0 && vdev_of_zigbee(sa) >= 0) {
        s_pend_sa[i] = SA_NONE;
        pend_save();
        return;
    }
    still_here(sa, ieee, now);
}

// The very device whose place the open series keeps is back (vdev_seen() has moved it to its short address already):
// nothing to replace, its virtual device goes into programming mode for ETS. True: that was it.
static bool came_back(uint16_t sa, const uint8_t ieee[8], int64_t now)
{
    if (s_st != ST_OPEN || s_replace < 0 || vdev_by_ieee(ieee) != s_replace) {
        return false;
    }
    char t[12];
    ia_str(s_target, t);
    int r = s_replace;
    s_replace = -1;
    close_network(now, "the device that left is back");
    vdev_prog(r, true);
    s_slot = r;
    s_st = ST_PENDING;
    ESP_LOGI(TAG, "0x%04x is the device that had left: virtual device %d (%s) is in programming mode for ETS", sa, r,
             t);
    return true;
}

// A device joined anew and is none of ours. In an open series or window it is the candidate from here on: its announce
// is not waited for (point 2). At any other time it is asked to leave.
static void joined(const ev_t *e, int64_t now)
{
    uint16_t sa = e->a;
    if (s_have_cand && sa == s_cand && memcmp(s_cand_ieee, e->ieee, 8) == 0) {
        return;   // the candidate again
    }
    if (s_st == ST_OPEN || s_st == ST_WINDOW) {
        s_cand_window = s_st == ST_WINDOW;
        s_cand = sa;
        memcpy(s_cand_ieee, e->ieee, 8);
        s_cand_cap_known = false;
        s_have_cand = true;
        s_cand_t = now;
        s_st = ST_AUTH;
        close_network(now, "a device joined");
        ESP_LOGI(TAG, "0x%04x joined, waiting for its key exchange", sa);
    } else if (s_st != ST_IDLE || (s_closed && now - s_closed < GUARD_US)) {
        ask_to_leave(sa, e->ieee, "joined in the same window");
    }
}

// One of our devices joined as a device new to the network does (Device_Update "joined", not a rejoin): it was reset
// by hand. It stays with its virtual device, address and download; what the reset cleared in it (binding, reporting,
// what its class wrote into it) is set up again. While the network is open for a new device it is not taken for that
// one: whoever reset it to give it another address has to unload its address in ETS first.
static void reset_joined(const ev_t *e, bool taken_back)
{
    int i = vdev_by_ieee(e->ieee);
    if (i < 0) {
        return;
    }
    bool linked = zlink_reinterview(100 + i);   // instance 100 + n belongs to virtual device n (VDEV_GW_INST)
    bool waits = !taken_back && (s_st == ST_OPEN || s_st == ST_WINDOW || s_st == ST_AUTH);
    ESP_LOGW(TAG, "0x%04x joined anew, as a device that was reset does: it is the Zigbee device of virtual device %d "
             "(%s) and stays with it; %s%s", e->a, i, vdev_unbound(i) ? "no address yet" : "it has its address",
             linked ? "its binding, reporting and the parameters of its class are set up again"
                    : "it runs no class yet, nothing to set up",
             waits ? ". It is not the new device the open network waits for" : "");
}

static void on_update(const ev_t *e, int64_t now)
{
    // Device_Update status as in the APS: 0 secured rejoin, 1 joined, 2 left, 3 trust centre rejoin. Only 1 is a
    // device new to the network; 0 and 3 are members coming back.
    if (e->b == 2) {   // for a device behind a router this is the only word of its leaving: the parent tells it
        on_left(e->a, e->ieee);
        return;
    }
    pend_seen(e->a, e->ieee);
    bool ours = vdev_seen(e->a, e->ieee);
    if (e->b == 1) {
        pend_drop(e->ieee);   // whoever it was before: it joined anew, a device like any other
    }
    bool back = came_back(e->a, e->ieee, now);
    if (e->b == 1 && ours) {
        reset_joined(e, back);
    }
    if (back) {
        return;
    }
    if (e->b != 1) {
        still_here(e->a, e->ieee, now);
    } else if (!ours) {
        joined(e, now);
    }
}

static void on_announce(const ev_t *e, int64_t now)
{
    uint16_t sa = e->a;
    vdev_set_capability(e->ieee, e->cap);   // one of ours: whether it sleeps; nobody's: nothing happens
    cap_note(vdev_by_ieee(e->ieee), e->ieee, true, e->cap);
    pend_seen(sa, e->ieee);
    if (s_have_cand && sa == s_cand) {   // the candidate: the flags go to the virtual device it becomes
        s_cand_cap = e->cap;
        s_cand_cap_known = true;
        return;
    }
    if (came_back(sa, e->ieee, now) || still_here(sa, e->ieee, now)) {
        return;
    }
    vdev_seen(sa, e->ieee);   // one of ours under a new short address
}

static void on_authorized(uint16_t sa, bool ok, int64_t now)
{
    if (s_st != ST_AUTH || !s_have_cand || sa != s_cand) {
        return;
    }
    if (ok) {
        candidate_ok(now);
    } else {
        ask_to_leave(sa, s_cand_ieee, "failed the key exchange");
        s_have_cand = false;
        s_st = ST_IDLE;
    }
}

static void on_assigned(int slot, uint16_t ia, int64_t now)
{
    uint16_t za;
    uint8_t ep;
    char t[12];
    ia_str(ia, t);
    if (slot >= 0 && slot < KNX_VDEV_MAX) {
        unl_clear(slot);   // unloaded a moment ago and addressed again: it stays
    }
    if (vdev_zigbee(slot, &za, &ep)) {
        esp_err_t e = zb_identify(za, ep, IDENTIFY_S);
        ESP_LOGI(TAG, "virtual device %d is %s: 0x%04x identifies itself for %d s (%s)", slot, t, za, IDENTIFY_S,
                 esp_err_to_name(e));
    }
    if (s_st == ST_PENDING && slot == s_slot) {   // the series did what it was for
        s_n_bound++;
        s_born[slot] = 0;
        s_done_t = now;
        s_st = ST_DONE;
        s_armed = false;
    }
}

static void on_unloaded(int slot, uint16_t old, int64_t now)
{
    if (slot < 0 || slot >= KNX_VDEV_MAX) {
        return;
    }
    if (old == 0xFFFF) {   // it had none: a device that waits for its address goes on waiting, but not for ever
        if (vdev_unbound(slot) && !s_born[slot]) {
            s_born[slot] = now;
        }
        return;
    }
    char t[12];
    ia_str(old, t);
    s_unload[slot] = now;
    s_unload_ia[slot] = old;
    unl_save();
    ESP_LOGI(TAG, "ETS unloaded the address %s of virtual device %d: it is removed in %d s and its Zigbee device asked "
             "to leave, unless ETS gives it an address before", t, slot, (int)(UNLOAD_US / 1000000));
}

static void on_left(uint16_t sa, const uint8_t ieee[8])
{
    if (pend_drop(ieee)) {
        ESP_LOGI(TAG, "0x%04x left the network, as it was asked to", sa);
    }
    if (s_st == ST_AUTH && s_have_cand && sa == s_cand) {
        ESP_LOGW(TAG, "0x%04x left before its key exchange was through", sa);
        s_have_cand = false;
        s_st = ST_IDLE;
    }
    int i = vdev_by_ieee(ieee);   // by its IEEE address: the short address may be another device's by now
    if (i < 0) {
        i = vdev_of_zigbee(sa);
    }
    if (i >= 0 && vdev_unbound(i) && i != s_slot) {   // without an address and gone: nothing to wait for
        bool unloaded = s_unload[i] != 0;
        uint16_t old = s_unload_ia[i];
        vdev_del(i);
        s_born[i] = 0;
        s_cap_known[i] = false;
        unl_clear(i);   // or the device that gets this place next would be removed in its stead
        if (unloaded) {   // ETS unloaded it a moment ago and it left by itself: the removal that was coming anyway
            char t[12];
            ia_str(old, t);
            s_n_unloaded++;
            gw_note(GW_NOTE_REMOVED, old, 0, REMOVED_WHY);
            ESP_LOGW(TAG, "0x%04x left the network: virtual device %d (%s until ETS unloaded it) is removed", sa, i, t);
        } else {
            ESP_LOGI(TAG, "0x%04x left the network: virtual device %d, which had no address yet, is removed", sa, i);
        }
    } else if (i >= 0) {   // it has an address: keep everything, the next ETS question for it takes a new device
        vdev_gone(i);
    }
}

static void pair_task(void *arg)
{
    (void)arg;
    ev_t e;
    for (;;) {
        bool got = xQueueReceive(s_q, &e, pdMS_TO_TICKS(1000)) == pdTRUE;
        int64_t now = esp_timer_get_time();
        if (got) {
            switch (e.type) {
            case EV_PROBE: on_probe(&e, now); break;
            case EV_POLL: on_poll(e.a, now); break;
            case EV_ASSIGNED: on_assigned(e.a, e.b, now); break;
            case EV_UPDATE: on_update(&e, now); break;
            case EV_ANNOUNCE: on_announce(&e, now); break;
            case EV_AUTHORIZED: on_authorized(e.a, e.ok, now); break;
            case EV_LEFT: on_left(e.a, e.ieee); break;
            case EV_UNLOADED: on_unloaded(e.a, e.b, now); break;
            // the console asked it, at a short address this does not know: its next announce or rejoin asks again
            case EV_REMOVED: pend_add(e.ieee, SA_NONE, 0); break;
            case EV_PROG_ENDED:   // ETS has been reading for 4 min and wrote no address: this series is over
                if (s_st == ST_PENDING && e.a == (uint16_t)s_slot) {
                    end_series(now, "programming mode ended by itself");
                }
                break;
            case EV_HEARD:
                if (s_st == ST_CHECK && e.a == s_check_sa) {
                    s_heard = true;
                }
                pend_heard(e.a, now);
                break;
            case EV_OPENED:   // also with binding from ETS switched off: nobody opens this network but this firmware
                if (s_st == ST_OPEN || s_st == ST_WINDOW) {
                    // Our own opening holds the network open anyway and closes it when it ends. Closing here ended the
                    // window of a series whose state stayed "open" (2026-11-15).
                    ESP_LOGW(TAG, "the Zigbee network was opened for %u s without this gateway asking for it: it is "
                             "open for the %s anyway, which closes it", e.a,
                             s_st == ST_OPEN ? "address assignment" : "pairing window");
                    break;
                }
                s_n_reclosed++;
                ESP_LOGW(TAG, "the Zigbee network was opened for %u s without this gateway asking for it", e.a);
                close_network(now, "nobody here asked for it");
                break;
            case EV_WINDOW: on_window(e.a, now); break;
            }
        }
        on_tick(now);
    }
}

void pair_init(void)   // before Zigbee starts: what it reports until pair_start() waits in the queue
{
    if (!s_q) {
        s_q = xQueueCreate(16, sizeof(ev_t));
    }
}

void pair_start(void)   // after vdev_start(): the storage partition is up then
{
    if (nvs_open_from_partition(BOARD_DEVSRV_PART, "pair", NVS_READWRITE, &s_nvs) == ESP_OK) {
        s_nvs_ok = true;
        uint16_t lo, hi;
        uint8_t en;
        if (nvs_get_u16(s_nvs, "lo", &lo) == ESP_OK && nvs_get_u16(s_nvs, "hi", &hi) == ESP_OK && lo <= hi) {
            s_lo = lo;
            s_hi = hi;
        }
        if (nvs_get_u8(s_nvs, "on", &en) == ESP_OK) {
            s_enabled = en;
        }
    }
    pend_load();
    unl_load();
    int waiting = 0;
    for (int i = 0; i < KNX_VDEV_MAX; i++) {   // devices that waited for an address before the restart: the time they
        if (vdev_unbound(i) && !s_unload[i]) {  // have for it starts again, it does not end
            s_born[i] = esp_timer_get_time();
            waiting++;
        }
    }
    pair_init();
    xTaskCreate(pair_task, "pair", 4096, NULL, 5, NULL);
    char a[12], b[12];
    ia_str(s_lo, a);
    ia_str(s_hi, b);
    ESP_LOGI(TAG, "binding from ETS is %s, addresses %s .. %s; %d devices wait for an address, %u were asked to leave "
             "and have not been seen leaving", s_enabled ? "on" : "off", a, b, waiting, (unsigned)s_pend_n);
}

void pair_enable(bool on)   // off: a series that runs is not continued, its next question finds it switched off
{
    if (s_enabled == on) {   // gw.c sets it at every start: no flash write for nothing
        return;
    }
    s_enabled = on;
    if (s_nvs_ok) {
        nvs_set_u8(s_nvs, "on", on);
        nvs_commit(s_nvs);
    }
}

int pair_range(uint16_t lo, uint16_t hi)   // 0: taken, 1: first above last, 2: 0.0.0 and 15.15.255 are no addresses
{
    if (lo > hi) {
        return 1;
    }
    if (lo == 0 || hi == 0xFFFF) {
        return 2;
    }
    if (lo == s_lo && hi == s_hi) {
        return 0;
    }
    s_lo = lo;
    s_hi = hi;
    if (s_nvs_ok) {
        nvs_set_u16(s_nvs, "lo", lo);
        nvs_set_u16(s_nvs, "hi", hi);
        nvs_commit(s_nvs);
    }
    return 0;
}

void pair_print(void)
{
    static const char *const ST[] = {"idle", "network open, waiting for a device", "a device joined, key exchange",
                                     "asking a waiting device whether it is there",
                                     "a device is in programming mode for ETS", "address written",
                                     "pairing window open (object or button)"};
    char a[12], b[12];
    ia_str(s_lo, a);
    ia_str(s_hi, b);
    printf("pair: binding from ETS %s, addresses %s .. %s, %s; opened %lu, bound %lu, asked to leave %lu, removed "
           "after waiting %lu, questions not for us %lu, closed again after somebody else opened the network %lu, "
           "pairing windows %lu with %lu devices, events lost %u\n",
           s_enabled ? "on" : "off", a, b, ST[s_st], (unsigned long)s_n_open, (unsigned long)s_n_bound,
           (unsigned long)s_n_asked, (unsigned long)s_n_expired, (unsigned long)s_n_ignored,
           (unsigned long)s_n_reclosed, (unsigned long)s_n_window, (unsigned long)s_n_waiting,
           (unsigned)atomic_load(&s_dropped));
    printf("pair: removed after ETS unloaded the address %lu; asked to leave and not seen leaving %u (asked again when "
           "they show up: %lu times so far)\n", (unsigned long)s_n_unloaded, (unsigned)s_pend_n,
           (unsigned long)s_n_again);
}
