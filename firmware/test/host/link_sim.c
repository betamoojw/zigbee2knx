// Host harness for main/knx_link.c: the real receive/transmit task runs against a small model of the NCN5130 UART
// side and a virtual clock. What the model assumes about the chip is marked.
#define _GNU_SOURCE
#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "knx_link.c"

#define BIT_US   104
#define CHAR_US  1354   // 13 bit times per octet on the bus
#define FWD_US   1432   // start of an octet on the bus -> octet complete at the host (11 bits on the bus + 11 bits at 38400)
#define UARTB_US 286

static int64_t vt;                 // virtual time, microseconds
static jmp_buf g_done;
static int64_t g_t_end;
static int g_quiet;

// ---- octets on their way from the chip to the host
typedef struct { int64_t t; uint8_t b; } rxb_t;
static rxb_t g_rx[200000];
static int g_rx_n, g_rx_i;
static void rx_add(int64_t t, uint8_t b)
{
    int i = g_rx_n++;
    while (i > g_rx_i && g_rx[i - 1].t > t) { g_rx[i] = g_rx[i - 1]; i--; }
    g_rx[i].t = t; g_rx[i].b = b;
}

// ---- frames on the bus
typedef struct { int64_t start, end; int n; uint8_t f[300]; bool ours, host_ack; int64_t ack_margin; } busf_t;
static busf_t g_bus[4000];
static int g_bus_n;
static int bus_add(int64_t start, const uint8_t *f, int n, bool ours)
{
    busf_t *b = &g_bus[g_bus_n];
    memset(b, 0, sizeof *b);
    b->start = start; b->n = n; b->ours = ours; b->ack_margin = INT64_MIN;
    b->end = start + (int64_t)n * CHAR_US - 2 * BIT_US;
    memcpy(b->f, f, (size_t)n);
    for (int i = 0; i < n; i++) rx_add(start + (int64_t)i * CHAR_US + FWD_US, f[i]);
    return g_bus_n++;
}

// ---- stalls of the host (flash erase and the like)
static struct { int64_t a, b; } g_stall[16];
static int g_stall_n;
static void stall_apply(void)
{
    for (int i = 0; i < g_stall_n; i++) {
        if (vt >= g_stall[i].a && vt < g_stall[i].b) vt = g_stall[i].b;
    }
}

// ---- scheduled sends of the device server
static struct { int64_t t; uint16_t src, dst; bool group; uint8_t prio, len, tpdu[KNX_TPDU_MAX]; bool done; } g_send[64];
static int g_send_n;

// ---- what the host wrote, as the chip sees it
// off80: the other reading of datasheet p. 46 ("U_L_DataStart.req resets the offset"): code 80h, which is also index 64
// with offset 1, resets the offset after it is used
static struct { bool ack_others; int64_t con_delay; bool no_con; bool off80; } g_chip = {.ack_others = true};
static uint8_t g_txbuf[320];       // index up to 4 * 64 + 63 (U_L_DataOffset.req 08..0C)
static int g_tx_pending_svc = -1;
static int g_off;                  // assumption: the chip starts with offset 0 and keeps it until the next request
static unsigned long n_offset_req;
static unsigned long n_uackn, n_uackn_stray, n_uackn_own, n_frames_to_chip, n_start_while_pending;
static int64_t g_chip_busy_until;   // our last frame is done at this time (L_Data.con sent)
static int64_t g_ackn_dt_min = INT64_MAX, g_ackn_dt_max;

static uint8_t cs(const uint8_t *f, int n) { uint8_t x = 0; for (int i = 0; i < n; i++) x ^= f[i]; return (uint8_t)~x; }

static void chip_transmit(int64_t tw, const uint8_t *f, int n)
{
    // assumption: the chip sends when the line was idle for 50 bit times; scripted foreign frames go first
    int64_t dur = (int64_t)n * CHAR_US, cand = tw + 300;
    bool moved = true;
    while (moved) {
        moved = false;
        for (int i = 0; i < g_bus_n; i++) {
            busf_t *b = &g_bus[i];
            int64_t free_at = b->end + 15 * BIT_US + 11 * BIT_US + 50 * BIT_US;
            if (b->start < cand + dur + 50 * BIT_US && free_at > cand) { cand = free_at; moved = true; }
        }
    }
    uint8_t g[320];
    memcpy(g, f, (size_t)n);
    int tries = g_chip.ack_others ? 1 : 4;
    int64_t t = cand, end = 0;
    for (int k = 0; k < tries; k++) {
        if (k) { g[0] &= (uint8_t)~0x20; g[n - 1] = cs(g, n - 1); }
        int i = bus_add(t, g, n, true);
        end = g_bus[i].end;
        t = end + 50 * BIT_US;
    }
    int64_t con = end + 3000 + g_chip.con_delay;
    if (!g_chip.no_con) rx_add(con, g_chip.ack_others ? 0x8B : 0x0B);
    g_chip_busy_until = con;
}

static void chip_from_host(int64_t t, uint8_t b)
{
    if (g_tx_pending_svc >= 0) {
        int svc = g_tx_pending_svc;
        g_tx_pending_svc = -1;
        int idx = g_off << 6 | (svc & 0x3F);
        if ((svc & 0xC0) == 0x80) {           // U_L_DataStart / U_L_DataCont
            if (idx == 0 && t < g_chip_busy_until) n_start_while_pending++;
            g_txbuf[idx] = b;
            if (svc == 0x80 && g_chip.off80) g_off = 0;
        } else {                               // U_L_DataEnd: index of the checksum octet
            int n = idx + 1;
            g_txbuf[n - 1] = b;
            n_frames_to_chip++;
            chip_transmit(t, g_txbuf, n);
        }
        return;
    }
    if ((b & 0xF8) == 0x08 && (b & 7) <= 4) { // U_L_DataOffset.req: upper three bits of the index (p32, p40)
        g_off = b & 7;
        n_offset_req++;
        return;
    }
    if ((b & 0xF8) == 0x10) {                  // U_Ackn.req
        n_uackn++;
        busf_t *hit = NULL;
        for (int i = 0; i < g_bus_n; i++) {
            busf_t *f = &g_bus[i];
            if (t >= f->start && t <= f->end + 15 * BIT_US) hit = f;
        }
        if (!hit) {
            n_uackn_stray++;
        } else {
            if (hit->ours) n_uackn_own++;
            hit->host_ack = true;
            hit->ack_margin = hit->end + 15 * BIT_US - t;
            int64_t dt = t - UARTB_US - (hit->start + FWD_US);
            if (dt < g_ackn_dt_min) g_ackn_dt_min = dt;
            if (dt > g_ackn_dt_max) g_ackn_dt_max = dt;
        }
    } else if (b >= 0x80 || (b & 0xC0) == 0x40) {
        g_tx_pending_svc = b;
    }
}

// ---- stubs
void stub_log(char lvl, const char *tag, const char *fmt, ...)
{
    if (g_quiet) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stdout, "    log %c %s: ", lvl, tag);
    vfprintf(stdout, fmt, ap);
    fputc('\n', stdout);
    va_end(ap);
}
int64_t esp_timer_get_time(void)
{
    for (int i = 0; i < g_send_n; i++) {
        if (!g_send[i].done && g_send[i].t <= vt) {
            g_send[i].done = true;
            knx_link_send(g_send[i].src, g_send[i].dst, g_send[i].group, g_send[i].prio, g_send[i].tpdu, g_send[i].len);
        }
    }
    vt += 4;
    return vt;
}
esp_err_t uart_driver_install(int p, int rx, int tx, int qs, void *q, int fl) { return ESP_OK; }
esp_err_t uart_param_config(int p, const uart_config_t *c) { return ESP_OK; }
esp_err_t uart_set_pin(int p, int tx, int rx, int rts, int cts) { return ESP_OK; }
esp_err_t uart_set_rx_full_threshold(int p, int n) { return ESP_OK; }
esp_err_t uart_set_rx_timeout(int p, int n) { return ESP_OK; }
static bool g_boot = true;
int uart_write_bytes(int port, const void *src, size_t n)
{
    const uint8_t *b = src;
    if (g_boot) return (int)n;   // U_Reset.req, U_State.req
    for (size_t i = 0; i < n; i++) chip_from_host(vt + (int64_t)(i + 1) * UARTB_US, b[i]);
    return (int)n;
}
int uart_read_bytes(int port, void *buf, uint32_t n, TickType_t ticks)
{
    int64_t deadline = ticks ? (vt / 1000 + ticks) * 1000 : vt;
    if (g_rx_i < g_rx_n && g_rx[g_rx_i].t <= deadline) {
        if (g_rx[g_rx_i].t > vt) vt = g_rx[g_rx_i].t;
        stall_apply();
        *(uint8_t *)buf = g_rx[g_rx_i++].b;
        vt += 12;
        return 1;
    }
    vt = deadline;
    stall_apply();
    if (vt > g_t_end && g_rx_i >= g_rx_n) longjmp(g_done, 1);
    return 0;
}
esp_err_t uart_get_buffered_data_len(int port, size_t *n)   // octets that have arrived and are not read yet
{
    size_t c = 0;
    for (int i = g_rx_i; i < g_rx_n && g_rx[i].t <= vt; i++) c++;
    *n = c;
    return ESP_OK;
}
struct stub_queue { unsigned n, size, head, count; uint8_t *buf; };
UBaseType_t uxQueueSpacesAvailable(QueueHandle_t q);
QueueHandle_t xQueueCreate(UBaseType_t n, UBaseType_t size)
{
    QueueHandle_t q = calloc(1, sizeof *q);
    q->n = n; q->size = size; q->buf = calloc(n, size);
    return q;
}
BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t t)
{
    if (q->count == q->n) return pdFALSE;
    memcpy(q->buf + ((q->head + q->count) % q->n) * q->size, item, q->size);
    q->count++;
    return pdTRUE;
}
UBaseType_t uxQueueSpacesAvailable(QueueHandle_t q) { return q->n - q->count; }
BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t t)
{
    if (!q->count) return pdFALSE;
    memcpy(item, q->buf + q->head * q->size, q->size);
    q->head = (q->head + 1) % q->n;
    q->count--;
    return pdTRUE;
}
BaseType_t xTaskCreate(void (*fn)(void *), const char *name, uint32_t st_, void *arg, UBaseType_t prio, TaskHandle_t *h) { return pdTRUE; }
void vTaskDelay(TickType_t t) { vt += (int64_t)t * 1000; g_boot = false; }
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t h) { return 0; }

// ---- what reaches the device server
static struct { knx_frame_t f; int64_t t; } g_sink[4000];
static int g_sink_n;
static void sink_fn(const knx_frame_t *f)
{
    if (g_sink_n < 4000) { g_sink[g_sink_n].f = *f; g_sink[g_sink_n].t = vt; g_sink_n++; }
}

// ---- scenario helpers
static int std_frame(uint8_t *f, uint8_t ctrl, uint16_t src, uint16_t dst, bool group, const uint8_t *tpdu, int len)
{
    f[0] = ctrl; f[1] = (uint8_t)(src >> 8); f[2] = (uint8_t)src; f[3] = (uint8_t)(dst >> 8); f[4] = (uint8_t)dst;
    f[5] = (uint8_t)((group ? 0x80 : 0) | 0x60 | (len - 1));
    memcpy(f + 6, tpdu, (size_t)len);
    f[6 + len] = cs(f, 6 + len);
    return 7 + len;
}
static int ext_frame(uint8_t *f, uint16_t src, uint16_t dst, bool group, const uint8_t *tpdu, int len)   // len octets after the TPCI octet + 1
{
    f[0] = 0x3C; f[1] = (uint8_t)((group ? 0x80 : 0) | 0x60); f[2] = (uint8_t)(src >> 8); f[3] = (uint8_t)src;
    f[4] = (uint8_t)(dst >> 8); f[5] = (uint8_t)dst; f[6] = (uint8_t)(len - 1);
    memcpy(f + 7, tpdu, (size_t)len);
    f[7 + len] = cs(f, 7 + len);
    return 8 + len;
}
static void send_at(int64_t t, uint16_t src, uint16_t dst, bool group, uint8_t prio, const uint8_t *tpdu, int len)
{
    int i = g_send_n++;
    g_send[i].t = t; g_send[i].src = src; g_send[i].dst = dst; g_send[i].group = group; g_send[i].prio = prio;
    g_send[i].len = (uint8_t)len; g_send[i].done = false;
    memcpy(g_send[i].tpdu, tpdu, (size_t)len);
}

static void reset_all(void)
{
    vt = 0; g_rx_n = g_rx_i = 0; g_bus_n = 0; g_stall_n = 0; g_send_n = 0; g_sink_n = 0; g_tx_pending_svc = -1;
    n_uackn = n_uackn_stray = n_uackn_own = n_frames_to_chip = n_start_while_pending = n_offset_req = 0;
    g_off = 0;
    g_chip_busy_until = 0; g_ackn_dt_min = INT64_MAX; g_ackn_dt_max = 0;
    g_chip.ack_others = true; g_chip.con_delay = 0; g_chip.no_con = false; g_chip.off80 = false;
    memset(&st, 0, sizeof st);
    st.ack_min_us = UINT32_MAX;
    g_boot = true;
    if (s_txq) { s_txq->head = s_txq->count = 0; } else { s_txq = xQueueCreate(16, sizeof(txf_t)); }
    s_sink = sink_fn;
}

static void run(int64_t t_end)
{
    g_t_end = t_end;
    if (!setjmp(g_done)) knx_task(NULL);
}

static void counters(const char *what)
{
    printf("  %s: frames %lu (ext %lu), bad checksum %lu, cut %lu, other %lu, frame_state %lu, state.ind %lu | passed %lu, own %lu, "
           "repetitions dropped %lu | host acks %lu | tx %lu: ack %lu, no ack %lu, no con %lu, chip errors %lu, queue full %lu\n",
           what, (unsigned long)st.frames, (unsigned long)st.ext, (unsigned long)st.bad_cs, (unsigned long)st.trunc, (unsigned long)st.other,
           (unsigned long)st.frame_state, (unsigned long)st.state_ind, (unsigned long)st.passed, (unsigned long)st.own, (unsigned long)st.dup,
           (unsigned long)st.acks, (unsigned long)st.tx, (unsigned long)st.tx_ack, (unsigned long)st.tx_noack, (unsigned long)st.tx_timeout,
           (unsigned long)st.tx_err, (unsigned long)st.tx_full);
}

static const uint16_t MY_IA[] = {0xFF01, 0xFF02};
static const uint16_t MY_GA[] = {0xF601, 0xF604};
#define ETS 0xFFF1

static void s1(void)
{
    printf("S1 baseline: a T_Data_Connected to one of our addresses, our T_ACK back, acknowledged by the partner\n");
    reset_all();
    knx_link_ack_lists(MY_IA, 2, MY_GA, 2, true);
    uint8_t f[64], dd[2] = {0x43, 0x00}, ta = 0xC2;
    int n = std_frame(f, 0xB0, ETS, 0xFF01, false, dd, 2);
    int b = bus_add(400000, f, n, false);
    send_at(400000 + n * CHAR_US + 2000, 0xFF01, ETS, false, 0, &ta, 1);
    run(600000);
    counters("counters");
    printf("  U_Ackn.req %lld us after the control octet reached the host (measured on the stick: 6720..6934), %lld us before the "
           "acknowledge slot; frames to the device server: %d (own flags:", (long long)g_ackn_dt_min, (long long)g_bus[b].ack_margin, g_sink_n);
    for (int i = 0; i < g_sink_n; i++) printf(" %d", g_sink[i].f.own);
    printf(")\n");
}

static void s2(void)
{
    printf("S2 repetitions: the same frame four times (not acknowledged by us: address not on the list)\n");
    reset_all();
    knx_link_ack_lists(MY_IA, 2, MY_GA, 2, true);
    uint8_t f[64], c = 0x80;
    int n = std_frame(f, 0xB0, ETS, 0xFF09, false, &c, 1);
    int64_t t = 400000;
    for (int k = 0; k < 4; k++) {
        if (k) { f[0] = 0x90; f[n - 1] = cs(f, n - 1); }
        int b = bus_add(t, f, n, false);
        t = g_bus[b].end + 50 * BIT_US;
    }
    run(700000);
    counters("counters");
}

static void s3(void)
{
    printf("S3 a second, new telegram with the same content within a second, whose first transmission is disturbed\n");
    reset_all();
    knx_link_ack_lists(MY_IA, 2, MY_GA, 2, true);
    uint8_t f[64], g[64], w[2] = {0x00, 0x81};   // GroupValue_Write 1 to 31/6/1, e.g. a toggle or a dimming step
    int n = std_frame(f, 0xBC, 0x1105, 0xF601, true, w, 2);
    bus_add(400000, f, n, false);                // first telegram, complete
    memcpy(g, f, (size_t)n);
    g[7] ^= 0x10;                                // second telegram 500 ms later: first transmission with a bit error
    int b = bus_add(900000, g, n, false);
    f[0] = 0x9C; f[n - 1] = cs(f, n - 1);        // its repetition, intact
    int r = bus_add(g_bus[b].end + 50 * BIT_US, f, n, false);
    run(1200000);
    counters("counters");
    printf("  to the device server: %d of 2 telegrams; the repetition was acknowledged by the host: %s\n", g_sink_n, g_bus[r].host_ack ? "yes" : "no");
}

static void s4(int tail_byte, const char *what)
{
    printf("S4 a foreign extended frame of 64 octets for another device while one of our frames waits in the chip (%s)\n", what);
    reset_all();
    knx_link_ack_lists(MY_IA, 2, MY_GA, 2, true);
    uint8_t f[300], p[60], st1[2] = {0x00, 0x81}, st2[2] = {0x00, 0x80};
    for (int i = 0; i < 56; i++) p[i] = 0x41;   // 41h is none of the service codes the receive loop knows
    p[0] = 0x42; p[1] = 0x80;
    if (tail_byte >= 0) p[45] = (uint8_t)tail_byte;                       // octet 52 of the frame, behind the 40 the buffer takes
    int n = ext_frame(f, ETS, 0x1105, false, p, 56);
    int b = bus_add(400000, f, n, false);
    send_at(402000, 0xFF01, 0xF604, true, 3, st1, 2);                      // status of device 1, queued while the frame runs
    send_at(403000, 0xFF02, 0xF604, true, 3, st2, 2);                      // and of device 2
    run(900000);
    counters("counters");
    printf("  frame length %d octets, %lld ms on the bus; frames written to the chip %lu, of those begun while the previous one was "
           "still in the chip: %lu; U_Ackn.req sent %lu (stray %lu, during our own frame %lu)\n",
           n, (long long)((g_bus[b].end - g_bus[b].start) / 1000), n_frames_to_chip, n_start_while_pending, n_uackn, n_uackn_stray, n_uackn_own);
    printf("  to the device server:");
    for (int i = 0; i < g_sink_n; i++) {
        printf(" [%04x->%04x %s]", g_sink[i].f.src, g_sink[i].f.dst, g_sink[i].f.own ? "own" : "FOREIGN");
    }
    printf("\n");
}

static void s4_random(void)
{
    printf("S4r the same with 2000 random payloads (one of our frames waiting each time)\n");
    unsigned long bad_err = 0, bad_con = 0, bad_foreign = 0, bad_start = 0, any = 0, cut = 0, own_ack = 0;
    uint64_t r = 0x853c49e6748fea9bull;
    g_quiet = 1;
    for (int it = 0; it < 2000; it++) {
        reset_all();
        knx_link_ack_lists(MY_IA, 2, MY_GA, 2, true);
        uint8_t f[300], p[60], st1[2] = {0x00, 0x81}, st2[2] = {0x00, 0x80};
        for (int i = 0; i < 56; i++) { r ^= r << 13; r ^= r >> 7; r ^= r << 17; p[i] = (uint8_t)(r >> 24); }
        int n = ext_frame(f, ETS, 0x1105, false, p, 56);
        bus_add(400000, f, n, false);
        send_at(402000, 0xFF01, 0xF604, true, 3, st1, 2);
        send_at(403000, 0xFF02, 0xF604, true, 3, st2, 2);
        run(900000);
        bool foreign = false;   // one of our own frames handed on as foreign (the foreign extended frame is passed on now)
        for (int i = 0; i < g_sink_n; i++) foreign = foreign || (!g_sink[i].f.own && (g_sink[i].f.src == 0xFF01 || g_sink[i].f.src == 0xFF02));
        bool b = st.tx_err || st.tx_ack + st.tx_noack != 2 || foreign || n_start_while_pending;
        any += b; bad_err += st.tx_err != 0; bad_con += (st.tx_ack + st.tx_noack != 2); bad_foreign += foreign;
        bad_start += n_start_while_pending != 0; cut += st.trunc != 0; own_ack += n_uackn_own != 0;
    }
    g_quiet = 0;
    printf("  the receive buffer cut the frame in %lu of 2000 runs; transmit bookkeeping went wrong in %lu: \"chip error\" counted %lu, number of "
           "confirmations wrong %lu, next frame begun while the previous was still in the chip %lu, our own echo handed on as a foreign "
           "frame %lu, U_Ackn.req during our own frame %lu\n", cut, any, bad_err, bad_con, bad_start, bad_foreign, own_ack);
}

static void s5(void)
{
    printf("S5 an extended frame of 30 octets to one of our individual addresses and one to one of our group addresses\n");
    reset_all();
    knx_link_ack_lists(MY_IA, 2, MY_GA, 2, true);
    uint8_t f[300], p[60] = {0x42, 0x80};
    int n = ext_frame(f, ETS, 0xFF01, false, p, 22);
    int a = bus_add(400000, f, n, false);
    n = ext_frame(f, 0x1105, 0xF601, true, p, 22);
    int b = bus_add(500000, f, n, false);
    run(700000);
    counters("counters");
    printf("  acknowledged by the host: %s / %s; handed to the device server: %d\n", g_bus[a].host_ack ? "yes" : "no", g_bus[b].host_ack ? "yes" : "no", g_sink_n);
}

static void s6(int64_t stall_ms)
{
    printf("S6 the link task does not run for %lld ms in the middle of a frame addressed to us (flash erase), the sender repeats\n", (long long)stall_ms);
    reset_all();
    knx_link_ack_lists(MY_IA, 2, MY_GA, 2, true);
    uint8_t f[64], mw[16] = {0x42, 0x8C, 0x40, 0x10, 0x0B, 0x8B, 0x97, 0x90, 0xFF, 0x01, 0x60, 0x0B, 0x13, 0x33, 0x17, 0x55};
    int n = std_frame(f, 0xB0, ETS, 0xFF01, false, mw, 16);
    int b0 = bus_add(400000, f, n, false);
    g_stall[0].a = 400000 + 3 * CHAR_US + FWD_US + 200; g_stall[0].b = g_stall[0].a + stall_ms * 1000; g_stall_n = 1;
    f[0] = 0x90; f[n - 1] = cs(f, n - 1);
    int b1 = bus_add(g_bus[b0].end + 50 * BIT_US, f, n, false);            // first repetition
    int b2 = bus_add(g_bus[b1].end + 50 * BIT_US, f, n, false);            // second
    int b3 = bus_add(g_bus[b2].end + 50 * BIT_US, f, n, false);            // third
    uint8_t st1[2] = {0x00, 0x81};
    send_at(399000, 0xFF02, 0xF604, true, 3, st1, 2);                      // one of our frames is in the chip meanwhile
    run(900000);
    counters("counters");
    printf("  acknowledged in time: original %s, repetitions %s %s %s; handed to the device server: %d frame(s); frames begun while the "
           "previous was still in the chip: %lu\n",
           g_bus[b0].host_ack && g_bus[b0].ack_margin > UARTB_US ? "yes" : "no", g_bus[b1].host_ack && g_bus[b1].ack_margin > UARTB_US ? "yes" : "no",
           g_bus[b2].host_ack && g_bus[b2].ack_margin > UARTB_US ? "yes" : "no", g_bus[b3].host_ack && g_bus[b3].ack_margin > UARTB_US ? "yes" : "no",
           g_sink_n, n_start_while_pending);
}

static void s7(void)
{
    printf("S7 L_Data.con later than 600 ms (line busy), two of our frames queued\n");
    reset_all();
    knx_link_ack_lists(MY_IA, 2, MY_GA, 2, true);
    uint8_t st1[2] = {0x00, 0x81}, st2[2] = {0x00, 0x80}, f[300], p[250];
    memset(p, 0x22, sizeof p);
    int64_t t = 399000;
    for (int k = 0; k < 3; k++) {                                          // three foreign frames of 263 octets back to back
        int n = ext_frame(f, ETS, 0x1105, false, p, 249);
        int b = bus_add(t, f, n, false);
        t = g_bus[b].end + 15 * BIT_US + 11 * BIT_US + 50 * BIT_US;
    }
    send_at(400000, 0xFF01, 0xF604, true, 3, st1, 2);
    send_at(400500, 0xFF02, 0xF604, true, 3, st2, 2);
    run(2500000);
    counters("counters");
    printf("  frames written to the chip %lu, begun while the previous was still in the chip: %lu; U_Ackn.req during our own frame: %lu\n",
           n_frames_to_chip, n_start_while_pending, n_uackn_own);
    printf("  to the device server:");
    for (int i = 0; i < g_sink_n; i++) {
        printf(" [%04x->%04x %s]", g_sink[i].f.src, g_sink[i].f.dst, g_sink[i].f.own ? "own" : "FOREIGN");
    }
    printf("\n");
}

static void s8(void)
{
    printf("S8 twenty group telegrams from the device server at once, nobody on the line acknowledges them\n");
    reset_all();
    knx_link_ack_lists(MY_IA, 2, MY_GA, 2, true);
    g_chip.ack_others = false;
    for (int i = 0; i < 20; i++) {
        uint8_t v[2] = {0x00, (uint8_t)(0x80 | (i & 1))};
        send_at(400000 + i * 100, (uint16_t)(0xFF01 + i % 8), (uint16_t)(0xF610 + i), true, 3, v, 2);
    }
    run(3000000);
    counters("counters");
}

static void s9(int64_t stall_us)
{
    printf("S9 an 8-octet frame to us (T_Connect); the link task is held up for %lld us after the fifth octet\n", (long long)stall_us);
    reset_all();
    knx_link_ack_lists(MY_IA, 2, MY_GA, 2, true);
    uint8_t f[64], c = 0x80;
    int n = std_frame(f, 0xB0, ETS, 0xFF01, false, &c, 1);
    int b = bus_add(400000, f, n, false);
    g_stall[0].a = 400000 + 4 * CHAR_US + FWD_US + 100; g_stall[0].b = g_stall[0].a + stall_us; g_stall_n = 1;
    run(600000);
    counters("counters");
    printf("  U_Ackn.req sent: %s; it reached the chip %lld us %s the acknowledge slot began; frame handed on: %d\n", g_bus[b].host_ack ? "yes" : (n_uackn ? "yes, after the frame" : "no"),
           (long long)(g_bus[b].host_ack ? (g_bus[b].ack_margin < 0 ? -g_bus[b].ack_margin : g_bus[b].ack_margin) : 0),
           g_bus[b].host_ack && g_bus[b].ack_margin >= 0 ? "before" : "AFTER", g_sink_n);
    printf("  (U_Ackn.req written in total: %lu, of those outside any frame and its slot: %lu)\n", n_uackn, n_uackn_stray);
}

static void s10(bool off80)
{
    printf("S10 the device server sends a TPDU of 67 octets (A_Memory_Response with 63 data octets), then a standard frame "
           "(chip model: %s)\n", off80 ? "80h resets the offset after use" : "the offset stays until the next one");
    reset_all();
    g_chip.off80 = off80;
    knx_link_ack_lists(MY_IA, 2, MY_GA, 2, true);
    uint8_t t[KNX_TPDU_MAX], ta = 0xC2;
    t[0] = 0x42; t[1] = 0x7F; t[2] = 0x40; t[3] = 0x00;   // T_Data_Connected seq 0, A_Memory_Response 63 octets at 4000h
    for (int i = 4; i < KNX_TPDU_MAX; i++) t[i] = (uint8_t)(i * 7);
    send_at(400000, 0xFF01, ETS, false, 0, t, KNX_TPDU_MAX);
    send_at(400100, 0xFF01, ETS, false, 0, &ta, 1);
    bool refused = knx_link_send(0xFF01, ETS, false, 0, t, KNX_TPDU_MAX + 1);
    run(900000);
    counters("counters");
    uint8_t want[FRAME_MAX];
    int wn = ext_frame(want, 0xFF01, ETS, false, t, KNX_TPDU_MAX);
    want[0] = 0x30; want[wn - 1] = cs(want, wn - 1);   // priority system, as the device server sends
    int ours = 0, match = 0, std_ok = 0;
    for (int i = 0; i < g_bus_n; i++) {
        if (!g_bus[i].ours) continue;
        ours++;
        if (g_bus[i].n == wn && memcmp(g_bus[i].f, want, (size_t)wn) == 0) match++;
        if (g_bus[i].n == 8 && g_bus[i].f[0] == 0xB0 && g_bus[i].f[6] == 0xC2) std_ok++;
    }
    printf("  on the bus: %d frames of ours, the extended one octet for octet as built here: %s (%d octets), the standard one "
           "after it intact: %s; U_L_DataOffset.req written %lu times (expected 13: 0 before the first frame, 1 in front of each "
           "of the octets 64..74, 0 again); a TPDU of %d octets refused: %s; handed back as own: %d\n",
           ours, match == 1 ? "yes" : "NO", wn, std_ok == 1 ? "yes" : "NO", n_offset_req, KNX_TPDU_MAX + 1, refused ? "NO" : "yes",
           g_sink_n);
}

static void s11(void)
{
    printf("S11 an extended frame with an A_Memory_Write of 63 octets to one of our addresses, one of 64 data octets more\n");
    reset_all();
    knx_link_ack_lists(MY_IA, 2, MY_GA, 2, true);
    uint8_t f[300], t[300];
    t[0] = 0x42; t[1] = 0xBF; t[2] = 0x40; t[3] = 0x00;   // T_Data_Connected seq 0, A_Memory_Write 63 octets at 4000h
    for (int i = 4; i < 300; i++) t[i] = (uint8_t)(i * 13);
    int n = ext_frame(f, ETS, 0xFF01, false, t, KNX_TPDU_MAX);
    int a = bus_add(400000, f, n, false);
    n = ext_frame(f, ETS, 0xFF02, false, t, KNX_TPDU_MAX + 1);   // one octet longer than the link keeps
    int b = bus_add(600000, f, n, false);
    run(900000);
    counters("counters");
    bool same = g_sink_n >= 1 && g_sink[0].f.len == KNX_TPDU_MAX && g_sink[0].f.ext && memcmp(g_sink[0].f.tpdu, t, KNX_TPDU_MAX) == 0;
    printf("  the 75-octet frame: acknowledged by the host %s (%lld us before the acknowledge slot), handed on whole %s; the "
           "76-octet frame: acknowledged %s, frames handed on %d (expected 1), counted as too long to keep %lu\n",
           g_bus[a].host_ack ? "yes" : "NO", (long long)g_bus[a].ack_margin, same ? "yes" : "NO", g_bus[b].host_ack ? "YES" : "no",
           g_sink_n, (unsigned long)st.ext_long);
}

static void s12(void)
{
    printf("S12 an extended frame in another format (EFF 0100b, LTE zone) whose address equals one of our group addresses\n");
    reset_all();
    knx_link_ack_lists(MY_IA, 2, MY_GA, 2, true);
    uint8_t f[300], w[2] = {0x00, 0x81};
    int n = ext_frame(f, 0x1105, 0xF601, true, w, 2);
    f[1] |= 0x04; f[n - 1] = cs(f, n - 1);
    int b = bus_add(400000, f, n, false);
    n = ext_frame(f, 0x1105, 0xF601, true, w, 2);   // the same in format 0000b
    int c = bus_add(500000, f, n, false);
    run(700000);
    counters("counters");
    printf("  EFF 0100b: acknowledged %s, dropped as another format %lu; EFF 0000b: acknowledged %s; handed on %d (expected 1)\n",
           g_bus[b].host_ack ? "YES" : "no", (unsigned long)st.ext_other, g_bus[c].host_ack ? "yes" : "NO", g_sink_n);
}

int main(void)
{
    s1();
    s2();
    s3();
    s4(-1, "payload without any octet that looks like a service");
    s4(0x8B, "one payload octet is 8Bh, the code of a positive L_Data.con");
    s4(0x17, "one payload octet is 17h, the code of U_State.ind with protocol error");
    s4_random();
    s5();
    s6(20);
    s6(60);
    s7();
    s8();
    s9(0);
    s9(4000);
    s9(5500);
    s10(false);
    s10(true);
    s11();
    s12();
    return 0;
}
