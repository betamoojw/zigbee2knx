// Targeted cases against the real vdev.c (host harness). Output: plain text on stdout.
#define NO_MAIN
#include "vdev_fuzz.c"

static const uint8_t UNLOAD[10] = {4}, START[10] = {1}, DONE[10] = {2};

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
}

static void alloc_obj(int dev, int obj, uint32_t n, int fill, uint8_t val)
{
    uint8_t al[10] = {3, 0x0B, (uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n, (uint8_t)fill, val, 0, 0};
    prop_write5(dev, obj, al);
}

static void set_app(int dev, uint16_t app)   // PID 13 of object 4, as ETS writes it right before LoadCompleted
{
    uint8_t t[11] = {0x03, 0xD7, 4, 13, 0x10, 0x01, 0x00, 0xFA, (uint8_t)(app >> 8), (uint8_t)app, 0x01};
    feed(PARTNER[0], dev_ia(dev), false, false, t, 11);
}

static void tick_for(int64_t us)   // the timer part of vdev_task()
{
    for (int64_t t = 0; t < us; t += 100000) {
        g_now += 100000;
        xSemaphoreTake(s_mx, portMAX_DELAY);
        timers(g_now);
        xSemaphoreGive(s_mx);
    }
}

static size_t nvs_bytes(void)
{
    size_t n = 0;
    for (int k = 0; k < NKEY; k++) {
        if (g_nvs[k].b) n += g_nvs[k].n;
    }
    return n;
}

static void fresh(void)
{
    wipe_ram();
    for (int k = 0; k < NKEY; k++) { free(g_nvs[k].b); g_nvs[k].b = NULL; }
    g_nvs_fail = g_zlink_fail = 0;
    g_txfail_pct = 0;
    g_tpl_mode = 0;
    g_tchk_err = 0;
    s_test_refuse = 0;
    s_saves = 0;
    publish();
}

static void case_a(void)
{
    say("== A: segment RAM a bus participant can hold, and what a restart brings back");
    fresh();
    for (int i = 0; i < VDEV_MAX; i++) {
        int n = vdev_add(0xFFFF, 1, 0, NULL);
        vdev_set_ia(n, (uint16_t)(0xFF01 + n));
    }
    tick_for(2000000);
    say("8 fresh devices: segment RAM %zu B, records in NVS %zu B", pinned(), nvs_bytes());
    unsigned long f0 = g_frames;
    for (int i = 0; i < VDEV_MAX; i++) {
        for (int o = 1; o <= 5; o++) {
            prop_write5(i, o, START);
            alloc_obj(i, o, o == 4 ? BIG4_SIZE : SEG_SIZE, 0, 0);
        }
    }
    say("after %lu connectionless frames (LoadStart + allocation of the maximum, no fill): segment RAM %zu B", g_frames - f0, pinned());
    for (int i = 0; i < VDEV_MAX; i++) {
        for (int o = 1; o <= 5; o++) prop_write5(i, o, UNLOAD);
    }
    say("right after Unload of every object: segment RAM %zu B (kept for the allocation a partial download sends next), sizes are 0", pinned());
    tick_for(2000000 * 9);
    say("saved while unloaded: records in NVS %zu B, segment RAM %zu B", nvs_bytes(), pinned());
    tick_for(45000000);
    say("63 s after the Unload: segment RAM %zu B", pinned());
    for (int i = 0; i < VDEV_MAX; i++) {
        for (int o = 1; o <= 5; o++) {
            prop_write5(i, o, START);
            alloc_obj(i, o, o == 4 ? BIG4_SIZE : SEG_SIZE, 0, 0);
        }
    }
    tick_for(2000000 * 9);
    say("allocated again and left in Loading, then quiet: %lu saves, records in NVS %zu B", (unsigned long)s_saves, nvs_bytes());
    wipe_ram();
    for (int i = 0; i < VDEV_MAX; i++) restore(i);
    say("after a restart (restore): segment RAM %zu B; dev 0 object 4: state %u, size %u, PID 7 would read %u",
        pinned(), s_d[0].lo[4].state, (unsigned)s_d[0].lo[4].size, (unsigned)(s_d[0].lo[4].state != LS_UNLOADED ? s_d[0].lo[4].ref : 0));
    uint8_t v[12];
    int n = prop_read(&s_d[0], 4, 27, v);
    char h[32];
    hex(v, n, h);
    say("   PID 27 of that unloaded object reads %s (a device unloaded at run time reads 0000000000ffffff)", h);
}

static void case_b(void)
{
    say("== B: restore() when the allocation of one segment fails");
    fresh();
    int d = vdev_add(0xFFFF, 1, 0, NULL);
    vdev_set_ia(d, 0xFF01);
    static const int SZ[6] = {0, 14, 26, 14, 100, 0};
    for (int o = 1; o <= 4; o++) {
        prop_write5(d, o, START);
        alloc_obj(d, o, (uint32_t)SZ[o], 1, (uint8_t)(0xA0 + o));   // object n filled with A0h + n
    }
    set_app(d, 0xC001);
    for (int o = 4; o >= 1; o--) prop_write5(d, o, DONE);
    tick_for(3000000);
    say("saved: %lu save(s), %zu B; objects 1..4 hold a1.., a2.., a3.., a4..", (unsigned long)s_saves, nvs_bytes());
    for (int fail = 2; fail <= 3; fail++) {
        wipe_ram();
        g_alloc_fail_at = (size_t)fail;   // 1 = the record buffer, 2 = segment of object 1, 3 = of object 2
        bool ok = restore(d);
        g_alloc_fail_at = 0;
        char line[200];
        int p = snprintf(line, sizeof line, "allocation %d fails -> restore %s:", fail, ok ? "true" : "false");
        for (int o = 1; o <= 4; o++) {
            lobj_t *l = &s_d[d].lo[o];
            p += snprintf(line + p, sizeof line - (size_t)p, " obj %d state %u size %u first %02x last %02x;", o, l->state,
                          (unsigned)l->size, l->size ? l->seg[0] : 0, l->size ? l->seg[l->size - 1] : 0);
        }
        say("%s", line);
    }
}

static void case_c(void)
{
    say("== C: answer without T_ACK from the partner: repetitions and the end of the connection");
    fresh();
    int d = vdev_add(0xFFFF, 1, 0, NULL);
    vdev_set_ia(d, 0xFF01);
    uint8_t c = 0x80, dd[2] = {0x43, 0x00};
    int64_t t0 = g_now;
    feed(PARTNER[0], 0xFF01, false, false, &c, 1);
    unsigned long tx0 = g_tx_n;
    feed(PARTNER[0], 0xFF01, false, false, dd, 2);
    say("t=0.0 s: request acknowledged and answered (%lu frames out)", g_tx_n - tx0);
    unsigned long last = g_tx_n;
    for (int i = 0; i < 150 && s_d[d].c.on; i++) {
        g_last_tx_n = 0;
        tick_for(100000);
        if (g_tx_n != last) {
            say("t=%.1f s: %lu frame(s) out, first TPDU octet %02x%s", (double)(g_now - t0) / 1e6, g_tx_n - last, g_last_tx[0].tpdu[0],
                g_last_tx[0].tpdu[0] == 0x81 ? " (T_Disconnect)" : " (the answer again)");
            last = g_tx_n;
        }
    }
    say("connection %s after %.1f s, repetition counter %u", s_d[d].c.on ? "still open" : "closed", (double)(g_now - t0) / 1e6, s_d[d].c.retries);
}

static void case_d(void)
{
    say("== D: when does save() run");
    fresh();
    int d = vdev_add(0xFFFF, 1, 0, NULL);
    tick_for(3000000);
    unsigned long s0 = s_saves;
    vdev_prog(d, true);
    uint8_t w[4] = {0x00, 0xC0, 0xFF, 0x01};
    feed(PARTNER[0], 0, true, false, w, 4);   // A_IndividualAddress_Write 15.15.1
    say("address written: dirty %d", s_d[d].dirty);
    uint8_t g[2] = {0x00, 0x81};
    int64_t t0 = g_now;
    for (int s = 0; s < 3600; s++) {          // a foreign group telegram once a second, to a group none of ours uses
        feed(0x1105, 0x0A0A, true, false, g, 2);
        tick_for(1000000);
    }
    say("one foreign group telegram per second for %.0f s: %lu saves, dirty %d", (double)(g_now - t0) / 1e6, s_saves - s0, s_d[d].dirty);
    for (int s = 0; s < 1200; s++) {          // every 1.4 s
        feed(0x1105, 0x0A0A, true, false, g, 2);
        tick_for(1400000);
    }
    say("then one every 1.4 s for 1680 s: %lu saves, dirty %d", s_saves - s0, s_d[d].dirty);
    tick_for(1600000);
    say("then 1.6 s without a frame: %lu saves, dirty %d", s_saves - s0, s_d[d].dirty);
    // a connection on another device
    int e = vdev_add(0xFFFF, 1, 0, NULL);
    vdev_set_ia(e, 0xFF02);
    tick_for(3000000);
    s0 = s_saves;
    vdev_set_ia(d, 0xFF03);                   // dirty again
    uint8_t c = 0x80;
    feed(PARTNER[1], 0xFF02, false, false, &c, 1);
    t0 = g_now;
    for (int s = 0; s < 120; s++) {           // the partner keeps its connection to device e alive
        uint8_t dd[2] = {(uint8_t)(0x43 | s_d[e].c.rx_seq << 2), 0x00};
        feed(PARTNER[1], 0xFF02, false, false, dd, 2);
        uint8_t a = (uint8_t)(0xC2 | s_d[e].c.tx_seq << 2);
        feed(PARTNER[1], 0xFF02, false, false, &a, 1);
        tick_for(5000000);
    }
    say("a connection to another virtual device kept open for %.0f s: %lu saves of the dirty device, dirty %d, connection %s",
        (double)(g_now - t0) / 1e6, s_saves - s0, s_d[d].dirty, s_d[e].c.on ? "open" : "closed");
    uint8_t dc = 0x81;
    feed(PARTNER[1], 0xFF02, false, false, &dc, 1);
    tick_for(3000000);
    g_nvs_fail = 1;
    s0 = s_saves;
    unsigned long e0 = g_log['E'];
    vdev_set_ia(d, 0xFF05);   // a change while the storage refuses to write
    tick_for(3000000);
    say("a change while the storage refuses the write: dirty %d, %lu error line(s)", s_d[d].dirty, g_log['E'] - e0);
    tick_for(25000000);
    say("25 s later, still refusing: dirty %d, %lu error line(s) (one attempt every 10 s)", s_d[d].dirty, g_log['E'] - e0);
    g_nvs_fail = 0;
    tick_for(12000000);
    say("12 s after the storage works again: dirty %d, successful saves %lu", s_d[d].dirty, s_saves - s0 - (g_log['E'] - e0));
}

static void load_light(int d)   // KO 1 switch (write), KO 4 status (transmit), one group address each
{
    static const uint8_t ADDR[6] = {0, 2, 0xF6, 0x01, 0xF6, 0x04}, ASSOC[10] = {0, 2, 0, 1, 0, 1, 0, 2, 0, 4},
                         GO[10] = {0, 4, 0x17, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4F, 0x00};
    for (int o = 4; o >= 1; o--) prop_write5(d, o, UNLOAD);
    prop_write5(d, 3, START); alloc_obj(d, 3, 10, 1, 0);
    prop_write5(d, 1, START); alloc_obj(d, 1, 6, 1, 0);
    prop_write5(d, 2, START); alloc_obj(d, 2, 10, 1, 0);
    mem_write(d, 0x4800, GO, 10);
    mem_write(d, 0x4000, ADDR, 6);
    mem_write(d, 0x4400, ASSOC, 10);
    for (int o = 3; o >= 1; o--) prop_write5(d, o, DONE);
}

static void case_e(void)
{
    say("== E: a status value while the link's send queue is full");
    fresh();
    int d = vdev_add(0xFFFF, 1, 0, NULL);
    vdev_set_ia(d, 0xFF01);
    load_light(d);
    uint8_t on = 1, off = 0;
    vdev_ko_out(100 + d, 4, &off, 1, false, true); pump();
    say("status 0 with a free queue: group frames counted %lu, link took %lu", (unsigned long)s_d[d].grp_tx, g_tx_n);
    unsigned long tx0 = g_tx_n;
    g_txfail_pct = 100;
    vdev_ko_out(100 + d, 4, &on, 1, false, true); pump();
    g_txfail_pct = 0;
    say("status 1 while knx_link_send() refuses: device counts %lu group frames out, link took %lu more", (unsigned long)s_d[d].grp_tx, g_tx_n - tx0);
    tick_for(300000);
    say("0.3 s later with a free queue and nothing else happening: link took %lu more (the device tried again)", g_tx_n - tx0);
    vdev_ko_out(100 + d, 4, &on, 1, false, true); pump();
    say("the class reports status 1 once more: link took %lu more in all", g_tx_n - tx0);
    uint8_t rd[2] = {0x00, 0x00};
    feed(PARTNER[0], 0xF604, true, false, rd, 2);
    say("a GroupValue_Read on that address is answered: %lu more", g_tx_n - tx0);
}

static void case_f(void)
{
    say("== F: frames to 15.15.255 while no device is in programming mode");
    fresh();
    int a = vdev_add(0xFFFF, 1, 0, NULL);
    (void)a;
    vdev_add(0xFFFF, 1, 0, NULL);
    bool listed = false;
    for (int i = 0; i < g_ack_nia; i++) listed = listed || g_ack_ia[i] == 0xFFFF;
    say("two unprogrammed devices, programming mode off: 15.15.255 on the acknowledge list: %s (%d addresses)", listed ? "yes" : "no", g_ack_nia);
    unsigned long tx0 = g_tx_n;
    uint8_t c = 0x80, dd[2] = {0x43, 0x00};
    feed(PARTNER[0], 0xFFFF, false, false, &c, 1);
    feed(PARTNER[0], 0xFFFF, false, false, dd, 2);
    say("T_Connect + A_DeviceDescriptor_Read to 15.15.255: %lu frames answered from 15.15.255 (T_ACK, descriptor), device 0 connected: %d",
        g_tx_n - tx0, s_d[0].c.on);
}

static void case_g(void)
{
    say("== G: connection queue and a partner that does not wait");
    fresh();
    int d = vdev_add(0xFFFF, 1, 0, NULL);
    vdev_set_ia(d, 0xFF01);
    uint8_t c = 0x80;
    feed(PARTNER[0], 0xFF01, false, false, &c, 1);
    unsigned long w0 = g_log['W'];
    for (int i = 0; i < 6; i++) {
        uint8_t dd[2] = {(uint8_t)(0x43 | i << 2), 0x00};
        feed(PARTNER[0], 0xFF01, false, false, dd, 2);
    }
    say("six requests without a T_ACK for the answers: queue holds %u, %lu answers dropped with a warning, rx_seq %u", s_d[d].c.qc,
        g_log['W'] - w0, s_d[d].c.rx_seq);
}

int main(void)
{
    vdev_start();
    case_a();
    case_b();
    case_c();
    case_d();
    case_e();
    case_f();
    case_g();
    return 0;
}
