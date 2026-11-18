// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dirk Tostmann
// random octet streams and random frames into the real receive/transmit task (ASan/UBSan)
#define main link_sim_main
#include "link_sim.c"
#undef main
static uint64_t r = 0x9E3779B97F4A7C15ull;
static unsigned long g_lost;
static void lost_cb(const knx_frame_t *f)   // what the link gives up comes back whole
{
    if (f->len < 1 || f->len > KNX_TPDU_MAX || !f->own) { fprintf(stderr, "given up with len %u\n", f->len); abort(); }
    g_lost++;
}
static uint32_t rnd(void) { r ^= r << 13; r ^= r >> 7; r ^= r << 17; return (uint32_t)(r >> 20); }
int main(int argc, char **argv)
{
    int rounds = argc > 1 ? atoi(argv[1]) : 200;
    r ^= (uint64_t)(argc > 2 ? atoi(argv[2]) : 1) * 0xD6E8FEB86659FD93ull;
    g_quiet = 1;
    unsigned long frames = 0, passed = 0, bytes = 0, acks = 0, txs = 0, dups = 0, full = 0;
    for (int k = 0; k < rounds; k++) {
        reset_all();
        s_lost = lost_cb;
        uint16_t ia[KNX_ACK_IA_MAX], ga[KNX_ACK_GA_MAX];
        int nia = (int)(rnd() % (KNX_ACK_IA_MAX + 1)), nga = (int)(rnd() % (KNX_ACK_GA_MAX + 1));
        for (int i = 0; i < nia; i++) ia[i] = (uint16_t)(rnd() % 4 ? 0xFF00 + rnd() % 16 : rnd());
        for (int i = 0; i < nga; i++) ga[i] = (uint16_t)(rnd() % 4 ? 0xF600 + rnd() % 16 : rnd());
        knx_link_ack_lists(ia, nia, ga, nga, rnd() & 1);
        g_chip.ack_others = rnd() % 3 != 0;
        g_chip.no_con = rnd() % 16 == 0;
        g_chip.con_delay = rnd() % 8 == 0 ? (int64_t)(rnd() % 900000) : 0;
        int64_t t = 300000;
        for (int j = 0; j < 400 && g_bus_n < 3500 && g_rx_n < 150000; j++) {
            uint8_t f[300], p[260];
            int kind = (int)(rnd() % 10);
            for (int i = 0; i < 260; i++) p[i] = (uint8_t)rnd();
            if (kind < 4) {            // a standard frame, addresses from a small pool so that some are ours
                int len = 1 + (int)(rnd() % 16);
                int n = std_frame(f, (uint8_t)(0x90 | (rnd() & 0x2C)), (uint16_t)(rnd() % 3 ? 0xFF00 + rnd() % 16 : rnd()),
                                  (uint16_t)(rnd() % 3 ? (rnd() & 1 ? 0xFF00 : 0xF600) + rnd() % 16 : rnd()), rnd() & 1, p, len);
                if (rnd() % 8 == 0) f[rnd() % n] ^= (uint8_t)(1 << (rnd() % 8));   // bit error
                if (rnd() % 12 == 0) n = 1 + (int)(rnd() % n);                      // cut off
                int b = bus_add(t, f, n, false);
                t = g_bus[b].end;
                if (rnd() % 4 == 0) {  // repeated by its sender
                    f[0] &= (uint8_t)~0x20; f[n - 1] = cs(f, n - 1);
                    b = bus_add(t + 50 * BIT_US, f, n, false);
                    t = g_bus[b].end;
                }
            } else if (kind < 6) {     // an extended frame of any length
                int len = 1 + (int)(rnd() % (rnd() & 1 ? 30 : 250));
                int n = ext_frame(f, (uint16_t)rnd(), (uint16_t)(rnd() % 3 ? 0xFF00 + rnd() % 16 : rnd()), rnd() & 1, p, len);
                if (rnd() % 6 == 0) f[6] = (uint8_t)rnd();                          // length field that does not fit
                int b = bus_add(t, f, n, false);
                t = g_bus[b].end;
            } else if (kind < 8) {     // octets that are no frame at all
                int n = 1 + (int)(rnd() % 40);
                for (int i = 0; i < n; i++) rx_add(t + (int64_t)i * (rnd() % 3 ? CHAR_US : (int64_t)(rnd() % 9000)), p[i]);
                t += (int64_t)n * CHAR_US;
            } else {                   // the device server sends, now and then a TPDU for an extended frame
                int len = 1 + (int)(rnd() % (rnd() % 4 ? 16 : KNX_TPDU_MAX));
                if (g_send_n < 60) send_at(t - (int64_t)(rnd() % 40000), (uint16_t)(0xFF00 + rnd() % 16),
                                           (uint16_t)((rnd() & 1 ? 0xFF00 : 0xF600) + rnd() % 16), rnd() & 1, (uint8_t)(rnd() & 3), p, len);
            }
            if (rnd() % 30 == 0 && g_stall_n < 16) {   // the task is held up
                g_stall[g_stall_n].a = t - (int64_t)(rnd() % 20000); g_stall[g_stall_n].b = g_stall[g_stall_n].a + (int64_t)(rnd() % 80000); g_stall_n++;
            }
            t += 50 * BIT_US + (int64_t)(rnd() % 3 ? rnd() % 3000 : rnd() % 200000);
        }
        run(t + 3000000);
        for (int i = 0; i < g_sink_n; i++) {
            if (g_sink[i].f.len < 1 || g_sink[i].f.len > KNX_TPDU_MAX) { fprintf(stderr, "frame with len %u handed on\n", g_sink[i].f.len); return 2; }
        }
        frames += st.frames; passed += st.passed; bytes += st.bytes; acks += st.acks; txs += st.tx; dups += st.dup; full += st.tx_full;
    }
    printf("%d rounds: %lu octets in, %lu frames with a good checksum, %lu handed on, %lu repetitions dropped, %lu host acks, %lu frames sent, "
           "%lu refused (queue full), %lu given up and reported\n", rounds, bytes, frames, passed, dups, acks, txs, full, g_lost);
    return 0;
}
