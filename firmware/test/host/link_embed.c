// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dirk Tostmann
#define main link_sim_main
#include "link_sim.c"
#undef main
int main(void)
{
    printf("S10 a standard frame embedded in the payload of a foreign extended group frame (64 octets, to a group address that is not ours)\n");
    reset_all();
    knx_link_ack_lists(MY_IA, 2, MY_GA, 2, true);
    uint8_t f[300], p[60], inner[32], rst[2] = {0x03, 0x80};
    for (int i = 0; i < 56; i++) p[i] = 0x41;
    p[0] = 0x00; p[1] = 0x80;
    int m = std_frame(inner, 0xB0, 0x1234, 0xFF01, false, rst, 2);   // connectionless A_Restart to our device 15.15.1
    memcpy(p + 40, inner, (size_t)m);                                 // frame octets 47..55
    int n = ext_frame(f, 0x2101, 0x0A0A, true, p, 56);
    int b = bus_add(400000, f, n, false);
    run(700000);
    counters("counters");
    printf("  handed to the device server: %d frame(s)", g_sink_n);
    for (int i = 0; i < g_sink_n; i++) {
        printf(" [%04x->%04x %s len %u tpdu %02x%02x]", g_sink[i].f.src, g_sink[i].f.dst, g_sink[i].f.group ? "group" : "individual", g_sink[i].f.len, g_sink[i].f.tpdu[0], g_sink[i].f.tpdu[1]);
    }
    printf("; U_Ackn.req written during the foreign frame: %lu (the frame's real destination is 1/2/10)\n", n_uackn);
    (void)b;
    return 0;
}
