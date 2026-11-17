// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dirk Tostmann
// NCN5130 on UART1 (TUL32-C6: ESP TX GPIO4 -> NCN RXD, ESP RX GPIO5 <- NCN TXD, 38400 8E1).
//
// Data link layer for several individual addresses on one transceiver. No U_SetAddress is sent, so the chip
// acknowledges nothing on its own (datasheet p35/36). The host decides per frame and answers with U_Ackn.req
// (0001 0nba, p32/p47) as soon as the destination is known: for an individual address on the list, for a group
// address on the list, and for broadcast while the device server asks for it. The lists come from the device
// server (vdev.c) and start empty, so without virtual devices the link only listens.
//
// Sending: a frame is written as U_L_DataStart/Cont/End pairs (p39) without blocking the receive path, then the
// link waits for L_Data.con (p40). The chip returns every octet it sends; such a frame is passed on marked as own,
// which is how a group telegram of one virtual device reaches the other virtual devices. A frame given up without a
// confirmation (no L_Data.con in time, an error flag of the chip while it went out) is reported back (knx_link_lost):
// as far as the link knows it never reached the bus, and only the device server knows whether to send it again.
//
// Extended frames (L_Data_Extended, 03_02_02 2.2.5) are received, acknowledged and passed on like standard frames as
// long as their TPDU fits KNX_TPDU_MAX and their extended frame format (EFF, low nibble of the extended control field)
// is 0000b: other formats address LTE zones (2.2.5.5), which are not our group addresses. A longer or other one is
// counted, not acknowledged and dropped. A TPDU longer than a standard frame takes goes out as an extended frame. Its
// octets past index 63 need U_L_DataOffset.req (p32, p40): the service octets carry 6 index bits, the offset the
// upper 3. The datasheet says the chip keeps the offset until the next one (p40) and resets it when a new frame
// starts with U_L_DataStart.req (p46), which is code 80h, the same as index 64 with offset 1. So the offset goes in
// front of every octet from index 64 on, as the OpenKNX TPUart library does it (Transmitter.cpp); whichever way the
// chip reads 80h then, the octet lands at its place. The NCN5130 of a TUL32 took the offset once per frame as well
// (2026-11-14: 86 frames of 75 octets, every echo compared).
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "app.h"
#include "board.h"

static const char *TAG = "knx";

#define KNX_UART       BOARD_KNX_UART   // UART1 (board.h)
#define KNX_TX_PIN     BOARD_KNX_TX_PIN
#define KNX_RX_PIN     BOARD_KNX_RX_PIN
#define U_RESET_REQ    0x01
#define U_STATE_REQ    0x02
#define U_RESET_IND    0x03
#define U_ACKN_ADDR    0x11   // U_Ackn.req with a = addressed
#define U_L_DATA_START 0x80   // | index
#define U_L_DATA_END   0x40   // | index of the checksum octet
#define U_L_DATA_OFF   0x08   // | upper 3 bits of the index for the octets that follow (0000 1iii, p32)
#define L_DATA_CON     0x0B   // bit 7: acknowledged on the bus
#define FRAME_MAX      (8 + KNX_TPDU_MAX)   // extended frame: 7 header octets, the TPDU, checksum (standard: 23)
#define TX_PAIR_US     600    // pause between two service/octet pairs
#define TX_CON_US      600000 // budget for L_Data.con on a quiet line: the chip repeats an unacknowledged frame up to
                              // three times. Every octet from the chip means the line is busy and starts it again.
#define TX_MAX_US      5000000   // ... but not for longer than this
#define TX_RESERVE     4      // places in the send queue that group telegrams leave to management
#define GAP_US         6000   // a frame is contiguous; the next one starts at least 7 ms after its last octet
#define DUP_US         400000 // a sender repeats within about 0.1 s; a longer window would take a new telegram with the
                              // same content for a repetition
#define EXT_MAX        264    // longest extended frame: 9 + 255 octets

typedef struct {
    uint8_t n;               // octets including the checksum
    uint8_t f[FRAME_MAX];
} txf_t;

static uint16_t s_ia[KNX_ACK_IA_MAX], s_ga[KNX_ACK_GA_MAX];
static volatile int s_nia, s_nga;
static volatile bool s_bcast;
static TaskHandle_t s_task;
static QueueHandle_t s_txq;
static knx_sink_t s_sink, s_lost;

static struct {
    uint32_t bytes, frames, ext, ext_long, bad_cs, trunc, reset_ind, state_ind, frame_state, bus_ack, other;
    uint32_t acks, ack_late, ack_min_us, ack_max_us;
    uint32_t tx, tx_ack, tx_noack, tx_timeout, tx_full, tx_err, own, dup, passed, ext_passed, tx_ext, ext_other;
    uint32_t tx_lost;        // frames given up and reported back (knx_link_lost)
    uint8_t last_state;
    uint16_t last_src, last_dst;
    bool last_group;
    int64_t last_reset_us;   // when the last U_Reset.ind came
} st = {.ack_min_us = UINT32_MAX};

static bool on_list(const uint16_t *l, int n, uint16_t a)
{
    for (int i = 0; i < n; i++) {
        if (l[i] == a) {
            return true;
        }
    }
    return false;
}

static bool ack_wanted(uint16_t dst, bool group)
{
    if (!group) {
        return on_list(s_ia, s_nia, dst);
    }
    return dst == 0 ? s_bcast : on_list(s_ga, s_nga, dst);
}

// The first n octets of a received frame against the frame in flight. The chip clears the repeat flag (bit 5 of
// the control octet) when it repeats, and the checksum changes with it, so neither is compared.
static bool same_as_tx(const txf_t *t, const uint8_t *f, int n)
{
    if (n < 2 || n > t->n || ((f[0] ^ t->f[0]) & 0xDF)) {
        return false;
    }
    int end = n < t->n ? n : t->n - 1;   // without the checksum
    return memcmp(f + 1, t->f + 1, (size_t)(end - 1)) == 0;
}

// A frame of ours given up: back to the device server as the frame it was (own set), in the link task
static void given_up(const txf_t *t)
{
    bool ext = !(t->f[0] & 0x80);
    int h = ext ? 7 : 6;
    if (!s_lost || t->n < h + 2) {
        return;
    }
    knx_frame_t k = {.src = ext ? (uint16_t)(t->f[2] << 8 | t->f[3]) : (uint16_t)(t->f[1] << 8 | t->f[2]),
                     .dst = ext ? (uint16_t)(t->f[4] << 8 | t->f[5]) : (uint16_t)(t->f[3] << 8 | t->f[4]),
                     .group = ext ? (t->f[1] & 0x80) : (t->f[5] & 0x80),
                     .own = true,
                     .ext = ext,
                     .len = (uint8_t)(t->n - h - 1)};
    memcpy(k.tpdu, t->f + h, k.len);
    st.tx_lost++;
    s_lost(&k);
}

static void knx_task(void *arg)
{
    (void)arg;
    const uart_config_t c = {
        .baud_rate = 38400,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_EVEN,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(KNX_UART, 1024, 256, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(KNX_UART, &c));
    ESP_ERROR_CHECK(uart_set_pin(KNX_UART, KNX_TX_PIN, KNX_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    uart_set_rx_full_threshold(KNX_UART, 1);   // every byte at once: the host acknowledge has to be early
    uart_set_rx_timeout(KNX_UART, 1);
    uint8_t q = U_RESET_REQ;
    uart_write_bytes(KNX_UART, &q, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    q = U_STATE_REQ;
    uart_write_bytes(KNX_UART, &q, 1);
    ESP_LOGI(TAG, "NCN5130 on TX=%d RX=%d, 38400 8E1, no address set in the chip, host acknowledge from the lists",
             KNX_TX_PIN, KNX_RX_PIN);

    uint8_t f[FRAME_MAX];
    int n = 0, need = 0;
    bool ext = false, decided = false;
    int64_t last = 0, t0 = 0;
    enum { TX_IDLE, TX_WRITING, TX_WAIT_CON } txs = TX_IDLE;
    txf_t cur = {0};
    int txi = 0;
    int off = -1;   // index offset the chip holds (U_L_DataOffset.req); -1: not known, it keeps one from frame to frame
    bool off_sent = false;   // the offset went out right in front of the octet that comes next
    int64_t tx_next = 0, tx_deadline = 0, tx_cap = 0;
    bool own_passed = false;
    uint8_t prev[FRAME_MAX];   // last foreign frame passed on, without control octet and checksum
    int prev_n = 0;
    int64_t prev_t = 0;
    for (;;) {
        int64_t now = esp_timer_get_time();
        if (txs == TX_IDLE && xQueueReceive(s_txq, &cur, 0) == pdTRUE) {
            txs = TX_WRITING;
            txi = 0;
            tx_next = now;
            own_passed = false;
        }
        if (txs == TX_WRITING && now >= tx_next) {
            // The offset first: in front of every octet from index 64 on, and back to 0 before the next frame
            if (txi >> 6 != off || (txi >= 64 && !off_sent)) {
                off = txi >> 6;
                off_sent = true;
                uint8_t o = (uint8_t)(U_L_DATA_OFF | off);
                uart_write_bytes(KNX_UART, &o, 1);
            } else {
                off_sent = false;
                uint8_t p[2] = {(uint8_t)((txi == cur.n - 1 ? U_L_DATA_END : U_L_DATA_START) | (txi & 0x3F)),
                                cur.f[txi]};
                uart_write_bytes(KNX_UART, p, 2);
                if (++txi == cur.n) {
                    txs = TX_WAIT_CON;
                    tx_deadline = now + TX_CON_US;
                    tx_cap = now + TX_MAX_US;
                    st.tx++;
                    st.tx_ext += cur.f[0] >> 7 == 0;
                }
            }
            tx_next = now + TX_PAIR_US;
        } else if (txs == TX_WAIT_CON && now > tx_deadline) {
            st.tx_timeout++;
            given_up(&cur);
            txs = TX_IDLE;
        }
        // Octets that came in while this task was not reading have no arrival time of their own: the gap rule
        // below would take a stall of the task for the end of a frame.
        size_t waiting = 0;
        uart_get_buffered_data_len(KNX_UART, &waiting);
        uint8_t b;
        int r = uart_read_bytes(KNX_UART, &b, 1, txs == TX_WRITING ? 1 : pdMS_TO_TICKS(2));
        now = esp_timer_get_time();
        if (r != 1) {
            if (n && now - last > 2 * GAP_US) {
                size_t behind = 0;   // octets came in while this task was held up after the timeout: no silence
                uart_get_buffered_data_len(KNX_UART, &behind);
                if (!behind) {
                    st.trunc++;
                    n = 0;
                }
            }
            continue;
        }
        st.bytes++;
        if (n && !waiting && now - last > GAP_US) {
            size_t behind = 0;   // more octets here already: this task was held up, the line was not silent
            uart_get_buffered_data_len(KNX_UART, &behind);
            if (!behind) {       // the rest of that frame never came
                st.trunc++;
                n = 0;
            }
        }
        last = now;
        if (txs == TX_WAIT_CON && now < tx_cap) {   // the line is busy: the chip may still be waiting for its turn
            tx_deadline = now + TX_CON_US;
        }
        if (n == 0) {
            need = 0;
            decided = false;
            if ((b & 0x7F) == L_DATA_CON) {
                if (txs == TX_WAIT_CON) {
                    if (b & 0x80) {
                        st.tx_ack++;
                    } else {
                        st.tx_noack++;
                    }
                    txs = TX_IDLE;
                } else {
                    st.other++;
                }
            } else if ((b & 0xD3) == 0x90 || (b & 0xD3) == 0x10) {   // L_Data_Standard.ind / L_Data_Extended.ind
                ext = (b & 0x80) == 0;
                f[n++] = b;
                t0 = now;
            } else if (b == U_RESET_IND) {
                st.reset_ind++;
                st.last_reset_us = esp_timer_get_time();
                off = -1;   // whatever the chip held before its reset
                if (st.reset_ind > 1) {   // the first one answers our U_Reset.req at the start
                    ESP_LOGW(TAG, "NCN5130 reset indication %lu at t=%lld s, not asked for",
                             (unsigned long)st.reset_ind, (long long)(st.last_reset_us / 1000000));
                }
            } else if ((b & 0x1F) == 0x13) {   // U_FrameState.ind
                st.frame_state++;
            } else if ((b & 0x07) == 0x07) {   // U_State.ind
                st.state_ind++;
                st.last_state = b;
                // sc, re, te, pe (datasheet p. 33-34); bit 3 is the temperature warning and says nothing about a frame
                if ((b & 0xF0) && txs != TX_IDLE) {   // an error flag while a frame is on its way: give it up
                    st.tx_err++;
                    given_up(&cur);
                    txs = TX_IDLE;
                }
            } else if (b == 0xCC || b == 0x0C || b == 0xC0) {   // ACK / NACK / BUSY seen on the bus
                st.bus_ack++;
            } else {
                st.other++;
            }
            continue;
        }
        if (n < (int)sizeof f) {   // a frame longer than the buffer is counted through to its end, not stored
            f[n] = b;
        }
        n++;
        if (!decided && ((!ext && n == 6) || (ext && n == 7))) {
            uint16_t dst = ext ? (uint16_t)(f[4] << 8 | f[5]) : (uint16_t)(f[3] << 8 | f[4]);
            bool group = ext ? (f[1] & 0x80) : (f[5] & 0x80);
            need = ext ? 9 + f[6] : 8 + (f[5] & 0x0F);
            decided = true;
            bool mine = txs != TX_IDLE && same_as_tx(&cur, f, n);
            // A frame too long to keep, or an extended one in another format (LTE zones), is not handed on, so it is
            // not acknowledged either: its sender gives up.
            if (!mine && need <= (int)sizeof f && !(ext && (f[1] & 0x0F)) && ack_wanted(dst, group)) {
                size_t ahead = 0;
                uart_get_buffered_data_len(KNX_UART, &ahead);
                if ((int)ahead >= need - n) {
                    // The whole rest of the frame is here already: U_Ackn.req would reach the chip after the frame,
                    // where the datasheet does not say what it does with it. The sender repeats the frame.
                    st.ack_late++;
                } else {
                    uint8_t a = U_ACKN_ADDR;
                    uart_write_bytes(KNX_UART, &a, 1);
                    uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
                    st.acks++;
                    if (us < st.ack_min_us) st.ack_min_us = us;
                    if (us > st.ack_max_us) st.ack_max_us = us;
                }
            }
        }
        if (need && n >= need) {
            uint8_t x = 0;
            for (int i = 0; i < n && i < (int)sizeof f; i++) {
                x ^= f[i];
            }
            if (n > (int)sizeof f) {
                st.ext_long++;   // complete, but only its head was kept
            } else if (ext && (f[1] & 0x0F)) {
                st.ext_other++;  // extended frame format other than 0000b: LTE zone addressing, not for us
            } else if (x == 0xFF) {
                st.frames++;
                st.ext += ext;
                st.last_src = ext ? (uint16_t)(f[2] << 8 | f[3]) : (uint16_t)(f[1] << 8 | f[2]);
                st.last_dst = ext ? (uint16_t)(f[4] << 8 | f[5]) : (uint16_t)(f[3] << 8 | f[4]);
                st.last_group = ext ? (f[1] & 0x80) : (f[5] & 0x80);
                if (s_sink) {
                    bool own = txs != TX_IDLE && n == cur.n && same_as_tx(&cur, f, n);
                    bool dup = false;
                    if (own) {
                        st.own++;
                        dup = own_passed;   // the chip repeats an unacknowledged frame: pass it on once
                        own_passed = true;
                    } else if (!(f[0] & 0x20) && prev_n == n - 2 && now - prev_t < DUP_US &&
                               memcmp(prev, f + 1, (size_t)(n - 2)) == 0) {
                        dup = true;         // repeated by its sender although it had arrived here
                    }
                    if (dup) {
                        st.dup++;
                    } else {
                        // n == need, so the TPDU fits: 9 + length octets in an extended frame, at most FRAME_MAX
                        knx_frame_t k = {.src = st.last_src, .dst = st.last_dst, .group = st.last_group, .own = own,
                                         .ext = ext, .len = (uint8_t)(ext ? f[6] + 1 : (f[5] & 0x0F) + 1)};
                        memcpy(k.tpdu, f + (ext ? 7 : 6), k.len);
                        st.ext_passed += ext;
                        if (!own) {
                            prev_n = n - 2;
                            memcpy(prev, f + 1, (size_t)prev_n);
                            prev_t = now;
                        }
                        st.passed++;
                        s_sink(&k);
                    }
                }
            } else {
                st.bad_cs++;
            }
            n = 0;
        } else if (n > EXT_MAX) {   // cannot happen with need from the frame; the last line of defence
            st.trunc++;
            n = 0;
        }
    }
}

void knx_link_start(void)
{
    s_txq = xQueueCreate(16, sizeof(txf_t));
    xTaskCreate(knx_task, "knx", 3072, NULL, 20, &s_task);
}

void knx_link_sink(knx_sink_t sink)
{
    s_sink = sink;
}

void knx_link_lost(knx_sink_t lost)
{
    s_lost = lost;
}

bool knx_link_send(uint16_t src, uint16_t dst, bool group, uint8_t prio, const uint8_t *tpdu, uint8_t len)
{
    if (!s_txq || !tpdu || len < 1 || len > KNX_TPDU_MAX) {
        return false;
    }
    txf_t t;
    uint8_t *f = t.f;
    int h;
    if (len <= KNX_TPDU_STD) {
        f[0] = (uint8_t)(0xB0 | (prio & 3) << 2);   // standard frame, not repeated
        f[1] = (uint8_t)(src >> 8);
        f[2] = (uint8_t)src;
        f[3] = (uint8_t)(dst >> 8);
        f[4] = (uint8_t)dst;
        f[5] = (uint8_t)((group ? 0x80 : 0x00) | 0x60 | (len - 1));   // hop count 6, octets after the TPCI octet
        h = 6;
    } else {   // extended frame (03_02_02 2.2.5, Figure 33): frame type bit 7 clear, then the extended control field
        f[0] = (uint8_t)(0x30 | (prio & 3) << 2);   // extended frame, not repeated
        f[1] = (uint8_t)((group ? 0x80 : 0x00) | 0x60);   // address type, hop count 6, extended frame format 0
        f[2] = (uint8_t)(src >> 8);
        f[3] = (uint8_t)src;
        f[4] = (uint8_t)(dst >> 8);
        f[5] = (uint8_t)dst;
        f[6] = (uint8_t)(len - 1);   // octets after the TPCI octet
        h = 7;
    }
    memcpy(f + h, tpdu, len);
    uint8_t x = 0;
    for (int i = 0; i < h + len; i++) {
        x ^= f[i];
    }
    f[h + len] = (uint8_t)~x;
    t.n = (uint8_t)(h + len + 1);
    if (group && dst != 0 && uxQueueSpacesAvailable(s_txq) <= TX_RESERVE) {
        st.tx_full++;   // the caller keeps the value and offers it again; management frames keep their places
        return false;
    }
    if (xQueueSend(s_txq, &t, 0) != pdTRUE) {
        st.tx_full++;
        ESP_LOGW(TAG, "send queue full, frame dropped");
        return false;
    }
    return true;
}

// The receive task reads the lists without a lock: shrink first, then fill, then publish the count. A frame that
// arrives in between misses its acknowledge and is repeated by its sender.
void knx_link_ack_lists(const uint16_t *ia, int nia, const uint16_t *ga, int nga, bool broadcast)
{
    nia = nia > KNX_ACK_IA_MAX ? KNX_ACK_IA_MAX : nia;
    nga = nga > KNX_ACK_GA_MAX ? KNX_ACK_GA_MAX : nga;
    s_nia = 0;
    s_nga = 0;
    if (nia > 0) {
        memcpy(s_ia, ia, (size_t)nia * sizeof *ia);
    }
    if (nga > 0) {
        memcpy(s_ga, ga, (size_t)nga * sizeof *ga);
    }
    s_bcast = broadcast;
    s_nia = nia;
    s_nga = nga;
}

static void ia_str(uint16_t a, char out[12])
{
    snprintf(out, 12, "%u.%u.%u", a >> 12, (a >> 8) & 0x0F, a & 0xFF);
}

void knx_link_print(void)
{
    char s[12], d[12];
    ia_str(st.last_src, s);
    if (st.last_group) {
        snprintf(d, sizeof d, "%u/%u/%u", st.last_dst >> 11, (st.last_dst >> 8) & 0x07, st.last_dst & 0xFF);
    } else {
        ia_str(st.last_dst, d);
    }
    printf("knx rx: %lu bytes, %lu frames (%lu extended, %lu more too long to keep), %lu bad checksum, %lu cut; "
           "reset.ind %lu, state.ind %lu (last 0x%02x), frame_state %lu, bus acks %lu, other %lu; last %s -> %s; "
           "last reset.ind at t=%lld s\n",
           (unsigned long)st.bytes, (unsigned long)st.frames, (unsigned long)st.ext, (unsigned long)st.ext_long,
           (unsigned long)st.bad_cs,
           (unsigned long)st.trunc, (unsigned long)st.reset_ind, (unsigned long)st.state_ind, st.last_state,
           (unsigned long)st.frame_state, (unsigned long)st.bus_ack, (unsigned long)st.other, s, d,
           (long long)(st.last_reset_us / 1000000));
    printf("knx rx: %lu passed on (%lu extended, %lu extended in another format dropped), %lu own, %lu repetitions "
           "dropped; host acks %lu (%lu..%lu us after the control octet), %lu left out because they would have come "
           "too late\n",
           (unsigned long)st.passed, (unsigned long)st.ext_passed, (unsigned long)st.ext_other, (unsigned long)st.own,
           (unsigned long)st.dup,
           (unsigned long)st.acks,
           (unsigned long)(st.acks ? st.ack_min_us : 0), (unsigned long)st.ack_max_us, (unsigned long)st.ack_late);
    printf("knx tx: %lu frames (%lu extended), %lu acknowledged, %lu not acknowledged, %lu without L_Data.con, %lu chip "
           "errors (%lu given up and reported back), %lu queue full; task stack free %u B\n",
           (unsigned long)st.tx, (unsigned long)st.tx_ext, (unsigned long)st.tx_ack, (unsigned long)st.tx_noack, (unsigned long)st.tx_timeout,
           (unsigned long)st.tx_err, (unsigned long)st.tx_lost, (unsigned long)st.tx_full,
           s_task ? (unsigned)uxTaskGetStackHighWaterMark(s_task) * sizeof(StackType_t) : 0);
}

void knx_link_ack_print(void)
{
    char a[12];
    printf("host ack: %d individual addresses", s_nia);
    for (int i = 0; i < s_nia; i++) {
        ia_str(s_ia[i], a);
        printf("%s%s", i ? ", " : " (", a);
    }
    printf("%s, %d group addresses", s_nia ? ")" : "", s_nga);
    for (int i = 0; i < s_nga; i++) {
        printf("%s%u/%u/%u", i ? ", " : " (", s_ga[i] >> 11, (s_ga[i] >> 8) & 0x07, s_ga[i] & 0xFF);
    }
    printf("%s, broadcast %s\n", s_nga ? ")" : "", s_bcast ? "yes" : "no");
}
