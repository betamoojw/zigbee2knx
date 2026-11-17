// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dirk Tostmann
// Zigbee-to-KNX gateway: one ESP32-C6 carries a Zigbee coordinator, KNX TP over the NCN5130, a device server and
// Berry. No Wi-Fi; console on USB-Serial-JTAG (TUL32) or UART0 (TULX32), see board.h.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "ep_pick.h"

void app_heap(const char *phase);
void console_start(void);

// knx_link.c: NCN5130 on UART1, data link layer for several individual addresses. No address is set in the chip;
// the host acknowledges from the lists the device server publishes, which start empty (then the link only listens).
#define KNX_ACK_IA_MAX 64
#define KNX_ACK_GA_MAX 256   // 32 devices with several group addresses each
// Longest TPDU the link keeps and sends: the TPCI octet and 66 more, an A_Memory_Write or A_Memory_Response with 63
// data octets (its count field has 6 bits). Up to 16 octets go in a standard frame, longer ones in an extended frame
// (L_Data_Extended, 03_02_02 2.2.5); the NCN5130 sends and receives those up to 263 octets (datasheet p. 40, 47).
// Longer frames are counted and dropped, and not acknowledged.
#define KNX_TPDU_MAX 67
#define KNX_TPDU_STD 16      // what fits into a standard frame
typedef struct {
    uint16_t src, dst;
    bool group;         // dst is a group address, 0 = broadcast
    bool own;           // a frame this firmware sent, as the transceiver returned it
    bool ext;           // it came as an extended frame
    uint8_t len;        // octets in tpdu
    uint8_t tpdu[KNX_TPDU_MAX];   // TPCI octet first
} knx_frame_t;
typedef void (*knx_sink_t)(const knx_frame_t *f);   // called in the link task: must not block
void knx_link_start(void);
void knx_link_sink(knx_sink_t sink);
// len up to KNX_TPDU_MAX; more than KNX_TPDU_STD goes out as an extended frame
bool knx_link_send(uint16_t src, uint16_t dst, bool group, uint8_t prio, const uint8_t *tpdu, uint8_t len);
void knx_link_ack_lists(const uint16_t *ia, int nia, const uint16_t *ga, int nga, bool broadcast);
void knx_link_print(void);
void knx_link_ack_print(void);
// octets a group object value may take: DPT 16 (14 octets) still fits a standard frame (APDU of 15 octets,
// 03_05_01 4.3.7, p. 46)
#define KNX_KO_VAL 14

// vdev.c: KNX device server, virtual System B devices (mask 07B0) on the TP bus; after them, in slot VDEV_GW, the
// gateway's own device (gw.c), which is always there and has no Zigbee device behind it
#define KNX_VDEV_MAX 32   // 32 after measuring the RAM (it was 8)
#define VDEV_GW      KNX_VDEV_MAX
#define VDEV_GW_INST (100 + VDEV_GW)
void vdev_start(void);
// slot 0.., -1: full. zaddr 0xFFFF: no Zigbee device. k: built-in class 0 or 1 to run until a download, -1: none.
// ieee: the Zigbee device's IEEE address, NULL: not known yet.
int  vdev_add(uint16_t zaddr, uint8_t ep, int k, const uint8_t *ieee);
bool vdev_del(int i);
bool vdev_prog(int i, bool on);
bool vdev_set_ia(int i, uint16_t ia);
void vdev_dump(int i, int obj);
void vdev_print(void);
void vdev_ko_out(int inst, int ko, const uint8_t *v, int n, bool initial, bool status);   // from the class
bool vdev_any_prog(void);                               // is any virtual device in programming mode?
int  vdev_first_unbound(void);                          // first device with a Zigbee link and no address, -1: none
bool vdev_has_zigbee(uint16_t zaddr);                   // does a virtual device serve this Zigbee device?
int  vdev_of_zigbee(uint16_t zaddr);                    // ... and which one, -1: none
int  vdev_free_slots(void);
int  vdev_by_ieee(const uint8_t ieee[8]);               // the virtual device of this Zigbee device, -1: none
bool vdev_seen(uint16_t zaddr, const uint8_t ieee[8]);  // a Zigbee device showed up: true if it is one of ours
bool vdev_unbound(int i);                               // a Zigbee device behind it and no individual address yet
bool vdev_unprogrammed(int i);                          // in use and without an individual address
bool vdev_ieee(int i, uint8_t ieee[8]);                 // the IEEE address of its Zigbee device; false: not known
bool vdev_zigbee(int i, uint16_t *zaddr, uint8_t *ep);  // the Zigbee device behind virtual device i
void vdev_model_event(int inst);                        // the device told its model: compare it with the template
void vdev_gone(int i);                                  // its Zigbee device left the network
bool vdev_is_gone(int i);
bool vdev_replace(int i, uint16_t zaddr, const uint8_t ieee[8]);   // a new Zigbee device takes the place
bool vdev_gw_prog(void);                                // is the gateway's own device in programming mode?
bool vdev_vprog(void);                                  // ... or one of the virtual devices? (for the LED)
void vdev_counts(int *bound, int *waiting);             // Zigbee devices with and without an individual address
void vdev_test_refuse(uint8_t err);                     // console: the next check at LoadCompleted fails with err
// The Zigbee device with this IEEE address announced itself with these MAC capability flags (bit 3: receiver on when
// idle). Kept in its record: a sleepy end device must not count as gone because it does not answer at once.
void vdev_set_capability(const uint8_t ieee[8], uint8_t capability);
void vdev_avail_note(int inst, bool on);               // from zlink.c: the device of this instance is off / back
int  vdev_max_apdu(int n);                              // console: PID 56 of every device (15 .. 66); 0 reads it
void vdev_set_endpoint(uint16_t zaddr, uint8_t ep);     // the interview found the device's function on endpoint ep
uint8_t vdev_endpoint_of(uint16_t zaddr);               // the endpoint the device at zaddr is used on, 0: none of ours
void vdev_set_capability_of(uint16_t zaddr, uint8_t capability);   // as vdev_set_capability, by short address
int  vdev_faulty(void);                                 // virtual devices whose application is in Error or not running
int  vdev_store_pct(void);                              // NVS entries of the records' partition in use, percent

// Why a load state machine went to Error: PID_ERROR_CODE (28), DPT_ErrorClass_System 20.011
// (KNX Standard v3.0.0, 03_05_01 4.2.28 p. 40; codes 03_07_02 p. 55). Only the codes this firmware gives.
enum {
    KNX_ERR_NONE = 0,
    KNX_ERR_CONFIG = 3,      // configuration fault: the content is not acceptable (signature, wrong application)
    KNX_ERR_SOFTWARE = 5,    // software fault: the template's Berry does not come up
    KNX_ERR_NO_RAM = 7,      // insufficient volatile memory
    KNX_ERR_ALLOC_0 = 8,     // memory allocation command with size 0 received
    KNX_ERR_CRC = 9,         // CRC error: the template is damaged
    KNX_ERR_TOO_LONG = 13,   // maximal table length exceeded
    KNX_ERR_LOAD_CMD = 14,   // undefined load command received
};

// gw.c: the gateway's own KNX device (application 0xC000 "Zigbee gateway"), its button S1 and its LED
void gw_start(void);                                       // after pair_start()
void gw_params(const uint8_t *p, size_t n, uint16_t ia);   // parameter segment of the gateway's download
void gw_ia(uint16_t ia);                                   // the gateway got another individual address
void gw_knx_write(int ko, const uint8_t *v, int n);        // a group write to one of its objects
void gw_print(void);
// What the gateway has to tell about itself: the last thing it refused or could not do. ia: the device it
// concerns (0xFFFF none), err: KNX_ERR_* for a load state that went to Error, else 0.
enum {
    GW_NOTE_REFUSED = 1,   // a download the gateway does not take: the load state went to Error, ETS sees it
    GW_NOTE_FAULT,         // a template that does not come up after a restart: the device runs no class
    GW_NOTE_MODEL,         // the Zigbee model is not among those the template names (reported only)
    GW_NOTE_FULL,          // no virtual device free for another Zigbee device: a device or an ETS question refused
    GW_NOTE_OFFLINE,       // a linked Zigbee device does not answer (zlink.c, avail.h)
    GW_NOTE_ONLINE,        // ... and answers again
    GW_NOTE_REMOVED,       // ETS unloaded the address of a virtual device: it is removed, its Zigbee device asked to leave
};
void gw_note(int kind, uint16_t ia, uint8_t err, const char *why);

// pair.c: binding a Zigbee device from ETS. ETS asking for a device in programming mode opens the Zigbee network; the
// first device that joins becomes a virtual device in programming mode and gets the address ETS writes.
void pair_init(void);                                  // before zb_start(): the queue for what Zigbee reports
void pair_start(void);
void pair_knx_probe(uint16_t src, uint16_t target, int own_slot);   // T_Connect; own_slot: our device there, -1
void pair_knx_poll(uint16_t src);                      // A_IndividualAddress_Read on the line
void pair_knx_assigned(int slot, uint16_t ia);         // a virtual device got its address
void pair_knx_unloaded(int slot, uint16_t old_ia);     // ETS wrote 15.15.255 over its address: the device is removed
void pair_knx_prog_ended(int slot);                    // its programming mode ended by itself after 4 min
void pair_zb_update(uint16_t short_addr, const uint8_t ieee[8], int status);   // 1: joined new
void pair_zb_announce(uint16_t short_addr, const uint8_t ieee[8], uint8_t capability);   // MAC capability flags
void pair_zb_heard(uint16_t short_addr);                // a ZCL frame came from that device
void pair_zb_authorized(uint16_t short_addr, bool ok);
void pair_zb_left(uint16_t short_addr, const uint8_t ieee[8]);
// A Zigbee device was asked to leave because its virtual device was removed (console). If the request did not reach
// it, it is asked again when it shows up as a member; a leave indication or a new join ends that.
void pair_removed(const uint8_t ieee[8]);
void pair_zb_opened(uint8_t seconds);                  // the network opened without this firmware asking for it
void pair_enable(bool on);
int  pair_range(uint16_t lo, uint16_t hi);             // individual addresses that belong to this gateway; 0: taken
void pair_window(int seconds);                         // open the network; the first device that joins waits for ETS.
                                                       // 0 closes a window that is open.
bool pair_is_open(void);                               // the network is open because this gateway opened it
void pair_print(void);

// zb.c: Zigbee coordinator, native esp-zigbee-lib 2.x API
void      zb_start(void);
void      zb_print(void);
esp_err_t zb_permit_join(uint8_t seconds);
esp_err_t zb_permit_join_as(uint8_t seconds, bool own);   // own false: as if a device on the network had opened it
bool      zb_factory_reset(void);   // erases the network and restarts; false: refused on this board (TULX32)
bool      zb_tx_power(const int8_t *set, int8_t *now);   // set NULL: only read; false: the stack is not up
// ZCL client from our endpoint to a device by short address; answers and reports show up in the log
esp_err_t zb_onoff(uint16_t dst, uint8_t ep, int what);   // 0 off, 1 on, 2 toggle
esp_err_t zb_level(uint16_t dst, uint8_t ep, uint8_t level, uint16_t tenths);   // Move to Level (with On/Off)
esp_err_t zb_read(uint16_t dst, uint8_t ep, uint16_t cluster, const uint16_t *attrs, int n);
esp_err_t zb_write_u8(uint16_t dst, uint8_t ep, uint16_t cluster, uint16_t attr, uint8_t type, uint8_t value);
esp_err_t zb_write_u16(uint16_t dst, uint8_t ep, uint16_t cluster, uint16_t attr, uint8_t type, uint16_t value);
// the device reports this attribute to us on its own: bind the cluster to our endpoint, configure reporting
esp_err_t zb_report(uint16_t dst, uint8_t ep, uint16_t cluster, uint16_t attr, uint8_t type, uint16_t min_s,
                    uint16_t max_s, uint32_t change);
esp_err_t zb_setup_reports(uint16_t dst, uint8_t ep);   // On/Off, Level, colour temperature, battery
// any attribute of up to 4 octets; manuf != 0: manufacturer-specific with that manufacturer code
esp_err_t zb_write_attr(uint16_t dst, uint8_t ep, uint16_t cluster, uint16_t attr, uint8_t type, uint32_t value,
                        uint8_t size, uint16_t manuf);
esp_err_t zb_level_move(uint16_t dst, uint8_t ep, bool up, uint8_t rate, bool with_onoff);
esp_err_t zb_level_stop(uint16_t dst, uint8_t ep, bool with_onoff);
esp_err_t zb_color_temp(uint16_t dst, uint8_t ep, uint16_t mired, uint16_t tenths);   // Move to Color Temperature
esp_err_t zb_color_xy(uint16_t dst, uint8_t ep, uint16_t x, uint16_t y, uint16_t tenths);   // Move to Color
esp_err_t zb_color_temp_move(uint16_t dst, uint8_t ep, int dir, uint16_t rate);   // dir > 0 warmer (more mired), < 0 cooler, 0 stop
esp_err_t zb_identify(uint16_t dst, uint8_t ep, uint16_t seconds);
esp_err_t zb_leave(uint16_t short_addr, const uint8_t *ieee);   // ZDO Mgmt_Leave without rejoin; ieee NULL: all zero
// the full interview (ep_pick.h): ZDO Active_EP_req and Simple_Desc_req, the answers go to zlink_disc_eps/_desc
esp_err_t zb_node_desc(uint16_t short_addr);   // its MAC capability goes to zlink_disc_node
esp_err_t zb_active_ep(uint16_t short_addr);
esp_err_t zb_simple_desc(uint16_t short_addr, uint8_t ep);

// zlink.c: a Berry instance (100 and up) linked to a real Zigbee device. dp_set from the class goes out as ZCL,
// attribute values from the device come back as dp_event, ko_status/ko_send go to the device server (vdev.c).
void      zlink_start(void);
int       zlink_add(int inst, int k, uint16_t addr, uint8_t ep);   // k: Berry slot (0 dimmable, 1 white spectrum, from 2 templates)
void      zlink_del(int inst);                                       // the VM forgets the instance, the device is free
void      zlink_pause(int inst);                                     // ... for now: the same device is linked again
void      zlink_reread(int inst);                                    // ask the device for its model again
void      zlink_announced(int inst);                                 // it announced itself at its address: read back
int       zlink_set(int inst, int k, uint16_t addr, uint8_t ep);   // add, or move an instance to another class/device
void      zlink_out(const char *json);                               // every line a Berry hook emits (bvm.c m_out)
void      zlink_attr(uint16_t addr, uint16_t cluster, uint16_t attr, uint8_t type, const void *val);
// a command the device sent to us (remote buttons); true: a linked instance takes it as dp_event "zcl.cmd"
bool      zlink_cmd(uint16_t addr, uint16_t cluster, uint8_t cmd, const uint8_t *payload, int len);
esp_err_t zlink_knx_write(int inst, int ko, const char *hex);       // as if the group object had been written
void      zlink_poll(int seconds);                                   // read linked devices back periodically, 0 stops
void      zlink_print(void);
// availability (avail.h): fed by the Zigbee task, decided in the link task
void      zlink_heard(uint16_t addr);                    // a frame came from this short address
void      zlink_sent(uint16_t addr, uint8_t status);     // APS confirm of our unicast to it, status 0 = delivered
void      zlink_sleepy(int inst, bool sleepy);           // its receiver is off when idle (from the device announce)
int       zlink_unavailable(void);                       // linked devices that are off now
bool      zlink_is_off(int inst);                        // the device of this instance does not answer
void      zlink_gone(int inst, bool gone);               // its device left the network (off, not asked) / is back
bool      zlink_reinterview(int inst);                   // read model and state again, binding and reporting anew
// the full interview: which endpoint the device has its function on (ep_pick.h); one device at a time
void      zlink_discover(uint16_t addr);                 // any task: ask this device for its endpoints
void      zlink_disc_node(uint16_t addr, uint8_t capability, int err);         // Zigbee task: Node_Desc answer
void      zlink_disc_eps(uint16_t addr, const uint8_t *eps, int n, int err);   // Zigbee task: Active_EP answer
void      zlink_disc_desc(uint16_t addr, const ep_desc_t *d, int err);         // Zigbee task: Simple_Desc answer

// kgt.c: the signed device template a product carries in its parameter segment (KGT2)
typedef struct {
    uint32_t text_off, text_len, params;   // from the trailer: where the text is, octets of application parameters
    uint32_t z, payload;                   // compressed and uncompressed size
    uint32_t us;                           // what the checks took
    char fpr[17], digest[17];              // signing key; content digest (SHA-256 of the payload, first 8 octets)
    uint8_t err;                           // refused: KNX_ERR_* for the load state Error (PID 28)
} kgt_info_t;
enum { KGT_OK = 0, KGT_NONE = 1, KGT_REFUSED = -1 };
// Parameter segment -> payload on the heap (the caller frees it). KGT_NONE: no trailer, a product without a
// template. KGT_REFUSED: a check failed, why says which; nothing is handed out then.
int kgt_open(const uint8_t *seg, size_t n, uint8_t **payload, size_t *plen, kgt_info_t *info, char *why, size_t wn);

// bvm.c: one Berry VM per template, one worker task for all of them. Slots 0 and 1 hold the two classes built into
// the firmware, the slots from 2 the templates that came with an ETS download (one VM per content digest).
// Devices of the same template share a VM, so the slots count different templates, not devices. Eight at once is
// more than the heap carries (measured 2026-11-13: about four classes at once), plus one while a device changes.
#define BVM_SLOTS (2 + 8 + 1)
enum { BVM_NO_TEMPLATE = -1, BVM_REFUSED = -2 };
esp_err_t bvm_start(void);
int       bvm_populate(int total);   // instances 1..total, alternating between the two built-in classes
void      bvm_sim_step(void);
void      bvm_print(void);
void      bvm_gc(void);              // collect now in every VM, print time and heap
void      bvm_set_rate(int pct);     // Berry GC step rate, 200 = collect at double usage
bool          bvm_line(int k, const char *fmt, ...);   // one request line into the VM of slot k, waits for it
unsigned long bvm_next_id(void);
const char   *bvm_class_model(int k);   // default zigbeeModel of a built-in class, "" for a template slot
const char   *bvm_class_seg(int k);     // default parameter segment of a built-in class (hex), "" for a template slot
// The template embedded in a parameter segment, checked and running: its slot. BVM_NO_TEMPLATE: the segment carries
// none. BVM_REFUSED: why says which check failed. Runs on the Berry task and waits for it.
int         bvm_template(const uint8_t *seg, size_t n, kgt_info_t *info, char *why, size_t wn);
// Would the template embedded in a parameter segment come up? The checks of bvm_template() up to the VM: checksum,
// signature, a free slot, the memory loading takes. free_extra: heap that goes back before it loads (the VM of the
// class the device runs alone). BVM_NO_TEMPLATE, BVM_REFUSED (why, info->err), or 0: it would (a VM with the same
// content may run already). Runs on the Berry task and waits for it.
int         bvm_template_check(const uint8_t *seg, size_t n, size_t free_extra, kgt_info_t *info, char *why,
                               size_t wn);
size_t      bvm_slot_heap(int k);       // heap the VM of slot k holds, 0: none loaded
int         bvm_vm_count(void);         // VMs loaded: built-in classes and templates
const char *bvm_slot_digest(int k);     // content digest of the template in slot k, "" for a built-in class or none
void        bvm_keep(uint32_t mask);    // delete the VMs of template slots whose bit is clear
// Is the model instance inst reported one of those the template in slot k names? 1 yes, 0 no, -1 nothing to compare
// (a built-in class, a template without a list), -2 the device has not told its model yet. model and count are
// filled for 0 and 1.
int         bvm_model_check(int k, int inst, char *model, size_t n, int *count);
bool        bvm_slot_ok(int k);
const char *bvm_slot_name(int k);

// devsrv.c: dummy of the KNX device server, one RAM record per virtual device
int  devsrv_resize(int n);
void devsrv_print(void);
