// Behaviour checks against the real vdev.c: what the device server answers (host harness)
#define main cases_main
#include "vdev_cases.c"
#undef main
static int g_pass, g_fail;
static void ck(bool ok, const char *what)
{
    if (ok) g_pass++; else { g_fail++; printf("FAIL: %s\n", what); }
}
static bool sent(int i, uint16_t dst, const char *hexs)   // i-th frame sent since the last feed()
{
    if (i >= g_last_tx_n) return false;
    char h[2 * KNX_TPDU_MAX + 1];
    for (int k = 0; k < g_last_tx[i].len; k++) sprintf(h + 2 * k, "%02x", g_last_tx[i].tpdu[k]);
    return g_last_tx[i].dst == dst && strcmp(h, hexs) == 0;
}
static void conn(uint16_t src, uint16_t dst, uint8_t tp) { feed(src, dst, false, false, &tp, 1); }
static void data(uint16_t src, uint16_t dst, int seq, const uint8_t *apdu, int n)
{
    uint8_t t[KNX_TPDU_MAX];
    memcpy(t, apdu, (size_t)n);
    t[0] = (uint8_t)(0x40 | seq << 2 | (t[0] & 3));
    feed(src, dst, false, false, t, n);
}
static void load4(int d, const uint8_t *s, int n)   // a whole download of object 4 alone
{
    prop_write5(d, 4, UNLOAD);
    prop_write5(d, 4, START);
    alloc_obj(d, 4, (uint32_t)n, 1, 0);
    mem_write(d, (uint32_t)n > SEG_SIZE ? BIG4_BASE : SEG_BASE + 3 * SEG_SIZE, s, n);
    set_app(d, 0xC001);
    prop_write5(d, 4, DONE);
}
static void reboot_clean(void)   // saved as timers() would, RAM gone, back as vdev_start() brings it
{
    for (int i = 0; i < VDEV_MAX; i++) if (s_d[i].used) save(&s_d[i]);
    wipe_ram();
    for (int i = 0; i < VDEV_MAX; i++) restore(i);
    ts_sweep();
    publish();
}
static int nvs_tparts(void)
{
    int n = 0;
    for (int k = 0; k < NKEY; k++) n += g_nvs[k].b && g_nvs[k].key[0] == 't';
    return n;
}
static void pread(int d, int obj, int pid, int count, int start)
{
    uint8_t t[6] = {0x03, 0xD5, (uint8_t)obj, (uint8_t)pid, (uint8_t)(count << 4 | start >> 8), (uint8_t)start};
    feed(PARTNER[0], dev_ia(d), false, false, t, 6);
}
int main(void)
{
    const uint16_t A = PARTNER[0], B = PARTNER[1], IA = 0xFF01;
    static const uint8_t DD[2] = {0x03, 0x00}, RST[2] = {0x03, 0x80};
    vdev_start();
    fresh();
    int d = vdev_add(0xFFFF, 1, 0, NULL);
    vdev_set_ia(d, IA);
    conn_t *c = &s_d[d].c;
    // transport layer
    conn(A, IA, 0x80);
    ck(c->on && c->remote == A && g_last_tx_n == 0, "T_Connect opens, nothing is sent");
    data(A, IA, 0, DD, 2);
    ck(g_last_tx_n == 2 && sent(0, A, "c2") && sent(1, A, "434007b0"), "seq 0: T_ACK, then the descriptor 07B0 with seq 0");
    data(A, IA, 0, DD, 2);
    ck(g_last_tx_n == 1 && sent(0, A, "c2") && c->qc == 1, "the same frame again: T_ACK only, not run twice");
    data(A, IA, 5, DD, 2);
    ck(g_last_tx_n == 1 && sent(0, A, "d7") && c->on, "wrong sequence number: T_NAK, connection stays");
    conn(B, IA, 0x80);
    ck(g_last_tx_n == 1 && sent(0, B, "81") && c->on && c->remote == A, "T_Connect of a second partner: T_Disconnect to it, first connection stays");
    data(B, IA, 1, DD, 2);
    ck(g_last_tx_n == 1 && sent(0, B, "81") && c->rx_seq == 1, "data of the second partner: T_Disconnect to it, not run");
    conn(B, IA, 0x81);
    ck(c->on, "T_Disconnect of the second partner is ignored");
    conn(A, IA, 0xC3);   // T_NAK seq 0 for our answer
    ck(g_last_tx_n == 1 && sent(0, A, "434007b0") && c->retries == 1, "T_NAK for the answer: sent again");
    conn(A, IA, 0xC2);   // T_ACK seq 0
    ck(c->qc == 0 && c->tx_seq == 1 && g_last_tx_n == 0, "T_ACK takes the answer off the queue");
    conn(A, IA, 0xC2);   // once more, now unexpected
    ck(!c->on && sent(0, A, "81"), "unexpected T_ACK: T_Disconnect, connection closed");
    data(A, IA, 1, DD, 2);
    ck(g_last_tx_n == 1 && sent(0, A, "81"), "data without a connection: T_Disconnect");
    conn(A, IA, 0x80);
    data(A, IA, 0, DD, 2);
    conn(A, IA, 0x80);
    ck(c->on && c->qc == 0 && c->rx_seq == 0 && c->tx_seq == 0, "T_Connect of the same partner starts over");
    s_d[d].prog = true;
    data(A, IA, 0, RST, 2);
    ck(sent(0, A, "c2") && !c->on && !s_d[d].prog, "A_Restart: acknowledged, connection gone, programming mode off");
    conn(A, IA, 0x81);
    ck(g_last_tx_n == 0, "T_Disconnect after the restart: nothing");
    feed(A, IA, false, false, DD, 2);
    ck(g_last_tx_n == 1 && sent(0, A, "034007b0"), "connectionless descriptor read is answered connectionless");
    {
        uint8_t t2[2] = {0x03, 0x02};
        feed(A, IA, false, false, t2, 2);
        ck(g_last_tx_n == 1 && sent(0, A, "037f"), "descriptor type 2: answer with type 3Fh, no data");
    }
    // load state machines and segments
    lobj_t *o1 = &s_d[d].lo[1], *o4 = &s_d[d].lo[4];
    prop_write5(d, 1, START);
    ck(o1->state == LS_LOADING && sent(0, A, "03d60105100102"), "LoadStart: Loading, answered with the state");
    alloc_obj(d, 1, 20, 1, 0xAA);
    ck(o1->size == 20 && o1->ref == 0x4000 && o1->seg[0] == 0xAA && o1->seg[19] == 0xAA, "allocation with fill fills");
    uint8_t w[4] = {1, 2, 3, 4};
    mem_write(d, 0x4000 + 16, w, 4);
    ck(o1->seg[16] == 1 && o1->seg[19] == 4, "memory write at the end of the segment");
    mem_write(d, 0x4000 + 17, w, 4);
    ck(o1->seg[17] == 2 && s_d[d].mem_rejected == 1, "memory write across the end is refused as a whole");
    {
        uint8_t r[4] = {0x02, 0x04, 0x40, 0x10};
        feed(A, IA, false, false, r, 4);
        ck(sent(0, A, "0244401001020304"), "memory read inside the segment");
        uint8_t r2[4] = {0x02, 0x05, 0x40, 0x10};
        feed(A, IA, false, false, r2, 4);
        ck(sent(0, A, "02404010"), "memory read across the end: count 0");
        uint8_t r3[4] = {0x02, 0x01, 0x3F, 0xFF};
        feed(A, IA, false, false, r3, 4);
        ck(sent(0, A, "02403fff"), "memory read one octet in front of the segment: count 0");
    }
    pread(d, 1, 27, 1, 1);
    char stamp_loading[40] = "";
    if (g_last_tx_n) for (int k = 0; k < g_last_tx[0].len; k++) sprintf(stamp_loading + 2 * k, "%02x", g_last_tx[0].tpdu[k]);
    prop_write5(d, 1, DONE);
    ck(o1->state == LS_LOADED, "LoadCompleted: Loaded");
    mem_write(d, 0x4000, w, 4);
    ck(o1->seg[0] == 0xAA, "memory write into a loaded object is refused");
    pread(d, 1, 27, 1, 1);
    ck(g_last_tx_n == 1 && g_last_tx[0].len == 14 && g_last_tx[0].tpdu[9] == 20 && g_last_tx[0].tpdu[10] == 0x00 && g_last_tx[0].tpdu[11] == 0xFF,
       "PID 27 while loaded: size 20, 00 FF, CRC");
    uint16_t crc_before = be16(g_last_tx[0].tpdu + 12);
    ck(crc_before == crc16_ccitt(o1->seg, 20), "PID 27 CRC is the CRC-16-CCITT of the segment");
    prop_write5(d, 1, UNLOAD);
    pread(d, 1, 27, 1, 1);
    ck(sent(0, A, "03d6011b10010000000000ffffff"), "PID 27 after Unload: size 0, CRC FFFF");
    pread(d, 1, 7, 1, 1);
    ck(sent(0, A, "03d601071001" "00000000"), "PID 7 after Unload: 0");
    prop_write5(d, 1, START);
    alloc_obj(d, 1, 20, 0, 0x55);
    ck(o1->seg[0] == 0xAA && o1->seg[19] == 4 && crc16_ccitt(o1->seg, 20) == crc_before, "allocation without fill after Unload: content and CRC as before");
    alloc_obj(d, 1, 0, 0, 0);
    ck(o1->state == LS_ERROR, "allocation of size 0: Error");
    prop_write5(d, 1, START);
    ck(o1->state == LS_ERROR, "LoadStart in Error stays Error");
    prop_write5(d, 1, DONE);
    ck(o1->state == LS_ERROR, "LoadCompleted in Error stays Error");
    prop_write5(d, 1, UNLOAD);
    prop_write5(d, 1, START);
    alloc_obj(d, 1, 1025, 0, 0);
    ck(o1->state == LS_ERROR, "table object: 1025 octets is Error");
    prop_write5(d, 1, UNLOAD);
    prop_write5(d, 1, START);
    alloc_obj(d, 1, 1024, 0, 0);
    ck(o1->state == LS_LOADING && o1->size == 1024, "table object: 1024 octets is taken");
    prop_write5(d, 4, START);
    alloc_obj(d, 4, 16385, 0, 0);
    ck(o4->state == LS_ERROR, "object 4: 16385 octets is Error");
    prop_write5(d, 4, UNLOAD);
    prop_write5(d, 4, START);
    alloc_obj(d, 4, 16384, 1, 0);
    ck(o4->state == LS_LOADING && o4->ref == 0x8000, "object 4: 16384 octets at 8000h");
    alloc_obj(d, 4, 1024, 0, 0);
    ck(o4->ref == 0x4C00 && o4->size == 1024, "object 4: 1024 octets at 4C00h");
    prop_write5(d, 4, UNLOAD);
    prop_write5(d, 4, START);
    g_alloc_fail_at = 1;
    alloc_obj(d, 2, 10, 0, 0);          // object 2 is unloaded: allocation outside Loading
    g_alloc_fail_at = 0;
    ck(s_d[d].lo[2].state == LS_ERROR, "allocation outside Loading: Error");
    prop_write5(d, 5, START);
    prop_write5(d, 5, DONE);
    ck(s_d[d].lo[5].state == LS_LOADED && s_d[d].lo[5].size == 0, "object 5: LoadStart, LoadCompleted without a segment");
    pread(d, 5, 27, 1, 1);
    ck(sent(0, A, "03d6051b10010000000000ffffff"), "object 5 PID 27: size 0, CRC FFFF");
    pread(d, 6, 1, 1, 1);
    ck(sent(0, A, "03d606010001"), "object 6 PID 1: absent (count 0)");
    pread(d, 0, 56, 1, 1);
    ck(sent(0, A, "03d600381001000f"), "PID 56 = 15");
    pread(d, 0, 56, 1, 0);
    ck(sent(0, A, "03d6003810000001"), "element 0 of a property: one element");
    pread(d, 0, 56, 2, 1);
    ck(sent(0, A, "03d600380001"), "two elements of a single-value property: count 0");
    // realloc failure keeps the old segment
    fresh();
    d = vdev_add(0xFFFF, 1, 0, NULL);
    vdev_set_ia(d, IA);
    o1 = &s_d[d].lo[1];
    prop_write5(d, 1, START);
    alloc_obj(d, 1, 8, 1, 0x11);
    g_alloc_fail_at = 1;
    alloc_obj(d, 1, 16, 1, 0x22);
    g_alloc_fail_at = 0;
    ck(o1->state == LS_ERROR && o1->size == 8 && o1->cap == 8 && o1->seg && o1->seg[7] == 0x11, "realloc failure: Error, old segment intact");
    // tables as arrays, group communication
    fresh();
    d = vdev_add(0xFFFF, 1, 0, NULL);
    vdev_set_ia(d, IA);
    load_light(d);
    pread(d, 1, 23, 1, 0);
    ck(sent(0, A, "03d6011710000002"), "PID_TABLE element 0: two group addresses");
    pread(d, 1, 23, 2, 1);
    ck(sent(0, A, "03d601172001f601f604"), "PID_TABLE elements 1..2");
    pread(d, 1, 23, 2, 2);
    ck(sent(0, A, "03d601170002"), "PID_TABLE beyond the end: count 0");
    pread(d, 2, 23, 3, 1);
    ck(sent(0, A, "03d602170001"), "three association entries do not fit a frame: count 0");
    ck(g_ack_nga == 2 && g_ack_nia == 1, "acknowledge lists: one address, two group addresses");
    {
        uint8_t wr[2] = {0x00, 0x81};
        unsigned long r0 = s_d[d].grp_rx;
        feed(0x1105, 0xF601, true, false, wr, 2);
        ck(s_d[d].grp_rx == r0 + 1, "group write to KO 1 reaches the device");
        feed(0x1105, 0xF604, true, false, wr, 2);
        ck(s_d[d].grp_rx == r0 + 2 && g_last_tx_n == 0, "group write to the status object (no write flag): counted, nothing done");
        uint8_t one = 1;
        vdev_ko_out(100 + d, 4, &one, 1, false, true); pump();
        ck(g_last_tx_n == 1 && g_last_tx[0].group && g_last_tx[0].dst == 0xF604 && g_last_tx[0].len == 2 && g_last_tx[0].tpdu[1] == 0x81, "status 1 goes out as GroupValue_Write 31/6/4 = 1");
        g_last_tx_n = 0;
        vdev_ko_out(100 + d, 4, &one, 1, false, true); pump();
        ck(g_last_tx_n == 0, "unchanged status is not sent again");
        uint8_t rd[2] = {0, 0};
        feed(0x1105, 0xF604, true, false, rd, 2);
        ck(g_last_tx_n == 1 && g_last_tx[0].tpdu[1] == 0x41, "GroupValue_Read is answered with the value");
        feed(IA, 0xF601, true, true, wr, 2);
        ck(s_d[d].grp_rx == r0 + 3, "an own frame of this device does not come back to it");
    }
    // broadcast
    fresh();
    d = vdev_add(0xFFFF, 1, 0, NULL);
    {
        uint8_t rd[2] = {0x01, 0x00}, wr[4] = {0x00, 0xC0, 0xFF, 0x05}, sr[8] = {0x03, 0xDC}, sw[14] = {0x03, 0xDE};
        feed(A, 0, true, false, rd, 2);
        ck(g_last_tx_n == 0, "A_IndividualAddress_Read outside programming mode: silent");
        feed(A, 0, true, false, wr, 4);
        ck(s_d[d].ia == 0xFFFF, "A_IndividualAddress_Write outside programming mode: ignored");
        vdev_prog(d, true);
        feed(A, 0, true, false, rd, 2);
        ck(g_last_tx_n == 1 && g_last_tx[0].src == 0xFFFF && g_last_tx[0].group && g_last_tx[0].dst == 0 && sent(0, 0, "0140"), "in programming mode: answer as broadcast");
        feed(A, 0, true, false, wr, 3);
        ck(s_d[d].ia == 0xFFFF, "address write cut to 3 octets: ignored");
        feed(A, 0, true, false, wr, 4);
        ck(s_d[d].ia == 0xFF05, "address written");
        memcpy(sr + 2, s_d[d].serial, 6);
        feed(A, 0, true, false, sr, 8);
        ck(g_last_tx_n == 1 && g_last_tx[0].len == 12 && g_last_tx[0].tpdu[1] == 0xDD && memcmp(g_last_tx[0].tpdu + 2, s_d[d].serial, 6) == 0, "serial number read: answer with 10 octets");
        feed(A, 0, true, false, sr, 7);
        ck(g_last_tx_n == 0, "serial number read cut short: silent");
        memcpy(sw + 2, s_d[d].serial, 6); sw[8] = 0xFF; sw[9] = 0x09;
        feed(A, 0, true, false, sw, 9);
        ck(s_d[d].ia == 0xFF05, "address by serial number cut to 9 octets: ignored");
        feed(A, 0, true, false, sw, 14);
        ck(s_d[d].ia == 0xFF09, "address by serial number");
        sw[7] ^= 1;
        sw[9] = 0x0A;
        feed(A, 0, true, false, sw, 14);
        ck(s_d[d].ia == 0xFF09, "another serial number: ignored");
        // ETS unloads an address by writing 15.15.255: pair.c is told with the address the device had
        uint8_t un[4] = {0x00, 0xC0, 0xFF, 0xFF};
        int as0 = g_assigned_n, un0 = g_unloaded_n;
        ck(as0 == 2 && un0 == 0, "the two addresses written above were reported as assigned, none as unloaded");
        feed(A, 0, true, false, un, 4);
        ck(s_d[d].ia == 0xFFFF && g_unloaded_n == un0 + 1 && g_unloaded_slot == d && g_unloaded_old == 0xFF09 &&
           g_assigned_n == as0, "15.15.255 written over an address: reported as unloaded with the old address, not as assigned");
        ck(s_d[d].used, "the device server itself removes nothing");
        sw[7] ^= 1;
        sw[8] = 0xFF; sw[9] = 0xFF;
        feed(A, 0, true, false, sw, 14);
        ck(g_unloaded_n == un0 + 2 && g_unloaded_old == 0xFFFF, "15.15.255 by serial number where there was none: reported with 15.15.255 as the old one");
        int gwi = VDEV_GW;
        bool gw_used = s_d[gwi].used;   // the gateway's device is made at the start of the device server
        s_d[gwi].used = true;
        s_d[gwi].ia = 0xFF30;
        s_d[gwi].prog = true;
        vdev_prog(d, false);
        feed(A, 0, true, false, un, 4);
        ck(s_d[gwi].ia == 0xFFFF && g_unloaded_n == un0 + 2, "the gateway's own device unloaded: its address goes, nothing is reported as unloaded");
        s_d[gwi].prog = false;
        s_d[gwi].used = gw_used;
    }
    // programming mode ends by itself after 4 min, however it was set (03_05_01 4.26.1)
    fresh();
    d = vdev_add(0xFFFF, 1, 0, NULL);
    {
        uint8_t rd[2] = {0x01, 0x00}, wr[4] = {0x00, 0xC0, 0xFF, 0x06};
        uint8_t on[7] = {0x03, 0xD7, 0x00, 54, 0x10, 0x01, 0x01};   // A_PropertyValue_Write, object 0, PID 54 = 1
        int n0 = g_prog_ended_n;
        vdev_prog(d, true);
        tick_for(239 * 1000000LL);
        feed(A, 0, true, false, rd, 2);
        ck(s_d[d].prog && g_last_tx_n == 1, "programming mode: after 239 s the address read is still answered");
        vdev_prog(d, true);   // set once more: the 4 min do not start again
        tick_for(2 * 1000000LL);
        feed(A, 0, true, false, rd, 2);
        ck(!s_d[d].prog && g_last_tx_n == 0, "after 4 min it is off by itself, also when it was set again in between");
        ck(g_prog_ended_n == n0 + 1 && g_prog_ended_slot == d, "the binding from ETS is told");
        feed(A, 0, true, false, wr, 4);
        ck(s_d[d].ia == 0xFFFF, "an address written afterwards is not taken");
        vdev_set_ia(d, 0xFF06);   // switched on through the property, as the ETS diagnostics do ("device LED on")
        conn(A, 0xFF06, 0x80);
        data(A, 0xFF06, 0, on, 7);
        ck(s_d[d].prog && s_d[d].prog_until > g_now, "PID 54 = 1 switches it on, with the same 4 min");
        conn(A, 0xFF06, 0x81);
        tick_for(241 * 1000000LL);
        ck(!s_d[d].prog && g_prog_ended_n == n0 + 2, "... and after them it is off");
        vdev_prog(d, true);
        vdev_prog(d, false);
        vdev_prog(d, true);
        tick_for(239 * 1000000LL);
        ck(s_d[d].prog, "switched off and on again: the 4 min count from the last switching on");
        int gwi = VDEV_GW;
        bool gw_used = s_d[gwi].used;
        s_d[gwi].used = true;
        vdev_prog(gwi, true);
        n0 = g_prog_ended_n;
        tick_for(241 * 1000000LL);
        ck(!s_d[gwi].prog && g_prog_ended_n == n0 + 1, "the gateway's own programming mode ends as well; only the device above is reported");
        s_d[gwi].used = gw_used;
    }
    // the template part of a parameter segment is kept once per content
    fresh();
    {
        int a = vdev_add(0xFFFF, 1, 0, NULL), b = vdev_add(0xFFFF, 1, 0, NULL);
        vdev_set_ia(a, 0xFF01);
        vdev_set_ia(b, 0xFF02);
        uint8_t sa[TPL_SEG_MAX], sb[TPL_SEG_MAX], back[TPL_SEG_MAX];
        int n = tpl_seg(0, sa);
        tpl_seg(0, sb);   // the same template, other parameters
        load4(a, sa, n);
        load4(b, sb, n);
        lobj_t *oa = &s_d[a].lo[4], *ob = &s_d[b].lo[4];
        ck(oa->ts1 && oa->ts1 == ob->ts1 && s_ts[oa->ts1 - 1].refs == 2 && oa->at == 10 && oa->cap == 10 && ob->cap == 10,
           "the same template in two devices: kept once, 10 octets of their own each");
        ck(seg_total() == 20 + (uint32_t)(n - 10), "segment RAM: two parameter parts and one template part");
        uint8_t mr[4] = {0x02, 0x0C, 0x4C, 0x04};   // 12 octets at 0x4C04: 6 of the parameters, 6 of the template
        g_last_tx_n = 0;
        feed(A, 0xFF01, false, false, mr, 4);
        ck(g_last_tx_n == 1 && g_last_tx[0].len == 16 && memcmp(g_last_tx[0].tpdu + 4, sa + 4, 12) == 0,
           "memory read across the end of the parameters: the octets ETS wrote");
        char exp[40];
        snprintf(exp, sizeof exp, "03d6041b1001%08x00ff%04x", (unsigned)n, crc16_ccitt(sa, (size_t)n));
        pread(a, 4, 27, 1, 1);
        ck(sent(0, A, exp), "PID 27: size and CRC over the whole segment");
        reboot_clean();
        oa = &s_d[a].lo[4];
        ob = &s_d[b].lo[4];
        seg_read(oa, 0, back, (uint32_t)n);
        ck(oa->state == LS_LOADED && oa->size == (uint32_t)n && memcmp(back, sa, (size_t)n) == 0 && oa->ts1 &&
           oa->ts1 == ob->ts1 && s_ts[oa->ts1 - 1].refs == 2, "after a restart: the same content, still kept once");
        size_t ra = 0;
        char key[4];
        snprintf(key, sizeof key, "v%d", a);
        nvs_get_blob(1, key, NULL, &ra);
        ck(nvs_tparts() == 1 && ra < (size_t)n, "NVS: the template part once, records smaller than the segment");
        prop_write5(a, 4, UNLOAD);
        prop_write5(a, 4, START);
        alloc_obj(a, 4, (uint32_t)n, 0, 0);
        ck(!oa->ts1 && oa->cap == (uint32_t)n && ob->ts1 && s_ts[ob->ts1 - 1].refs == 1,
           "allocation for a partial download: an own copy again, the other device keeps the part");
        static const uint8_t two[2] = {0xA5, 0x5A};
        mem_write(a, 0x4C00, two, 2);
        prop_write5(a, 4, DONE);
        sa[0] = 0xA5;
        sa[1] = 0x5A;
        seg_read(oa, 0, back, (uint32_t)n);
        ck(oa->ts1 == ob->ts1 && s_ts[oa->ts1 - 1].refs == 2 && memcmp(back, sa, (size_t)n) == 0,
           "partial download: two octets changed, the rest as before, kept once again");
        vdev_del(b);
        ck(s_ts[oa->ts1 - 1].refs == 1, "device deleted: one reference less");
        reboot_clean();
        ck(nvs_tparts() == 1, "after a restart the blob stays: the other device names it");
        vdev_del(a);
        reboot_clean();
        ck(nvs_tparts() == 0, "the last device gone: the blob goes at the next start");
        // a damaged blob
        a = vdev_add(0xFFFF, 1, 0, NULL);
        vdev_set_ia(a, 0xFF01);
        load4(a, sa, n);
        reboot_clean();
        for (int k = 0; k < NKEY; k++) {
            if (g_nvs[k].b && g_nvs[k].key[0] == 't') g_nvs[k].b[5] ^= 0x40;
        }
        reboot_clean();
        ck(s_d[a].used && s_d[a].ia == 0xFF01 && s_d[a].lo[4].state == LS_UNLOADED && !s_d[a].lo[4].ts1,
           "template part damaged: the device keeps its address, its application is unloaded");
        load4(a, sa, n);
        reboot_clean();
        seg_read(&s_d[a].lo[4], 0, back, (uint32_t)n);
        ck(s_d[a].lo[4].state == LS_LOADED && memcmp(back, sa, (size_t)n) == 0 && nvs_tparts() == 1,
           "loaded again: the blob is written anew and comes back whole");
        // no trailer, or one that points outside: nothing kept apart
        uint8_t sc[TPL_SEG_MAX];
        memcpy(sc, sa, (size_t)n);
        sc[n - 1] = 'G';
        load4(a, sc, n);
        ck(!s_d[a].lo[4].ts1 && s_d[a].lo[4].cap == (uint32_t)n, "trailer malformed: the segment stays whole");
        // replacing a device: the class forgets the one that left before it gets the new one
        s_d[a].zaddr = 0x1234;
        s_d[a].have_ieee = true;
        unsigned long del0 = g_zlink_del;
        static const uint8_t NEWIEEE[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        vdev_gone(a);
        ck(s_d[a].gone, "its Zigbee device left: marked");
        ck(vdev_replace(a, 0x5678, NEWIEEE) && s_d[a].zaddr == 0x5678 && !s_d[a].gone && g_zlink_del >= del0 + 1,
           "replace: the class forgets the device that left, the new one is linked");
    }
    // a value of 14 octets: DPT 16, the gateway's message object
    fresh();
    d = vdev_add(0xFFFF, 1, 0, NULL);
    vdev_set_ia(d, IA);
    {
        static const uint8_t AT[4] = {0, 1, 0xF6, 0x11}, AS[6] = {0, 1, 0, 1, 0, 11};   // 31/6/17 <-> KO 11
        uint8_t go[24] = {0, 11};
        go[22] = 0x4F;   // KO 11: communication, read, transmit
        go[23] = 14;     // type 14: 14 octets
        for (int o = 3; o >= 1; o--) prop_write5(d, o, UNLOAD);
        prop_write5(d, 3, START); alloc_obj(d, 3, sizeof go, 1, 0);
        prop_write5(d, 1, START); alloc_obj(d, 1, sizeof AT, 1, 0);
        prop_write5(d, 2, START); alloc_obj(d, 2, sizeof AS, 1, 0);
        mem_write(d, 0x4800, go, sizeof go);
        mem_write(d, 0x4000, AT, sizeof AT);
        mem_write(d, 0x4400, AS, sizeof AS);
        for (int o = 3; o >= 1; o--) prop_write5(d, o, DONE);
        uint8_t txt[14] = {'1', '5', '.', '1', '5', '.', '2', ' ', 'R', 'A', 'M'};
        g_last_tx_n = 0;
        vdev_ko_out(100 + d, 11, txt, 14, false, true); pump();
        ck(g_last_tx_n == 1 && g_last_tx[0].dst == 0xF611 && g_last_tx[0].len == 16 && g_last_tx[0].tpdu[1] == 0x80 &&
           memcmp(g_last_tx[0].tpdu + 2, txt, 14) == 0, "14 octets go out in one standard frame");
        uint8_t rd[2] = {0, 0};
        feed(0x1105, 0xF611, true, false, rd, 2);
        ck(g_last_tx_n == 1 && g_last_tx[0].len == 16 && g_last_tx[0].tpdu[1] == 0x40 &&
           memcmp(g_last_tx[0].tpdu + 2, txt, 14) == 0, "a read is answered with the 14 octets");
        uint8_t big[15] = {0};
        g_last_tx_n = 0;
        vdev_ko_out(100 + d, 11, big, 15, false, true); pump();
        ck(g_last_tx_n == 0 && s_d[d].ko[11].len == 14, "15 octets are refused, the value stays");
    }
    // what the gateway cannot take ends the download in Error, with the reason in PID 28
    fresh();
    {
        static const uint8_t SEG[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
        int a = vdev_add(0x1234, 1, 0, NULL);   // linked to a Zigbee device, runs built-in class 0
        vdev_set_ia(a, 0xFF01);
        ck(s_d[a].k == 0, "a device that runs a class");
        unsigned long del0 = g_zlink_del, ref0 = g_note_n[GW_NOTE_REFUSED];
        g_tchk_err = KNX_ERR_NO_RAM;
        prop_write5(a, 4, UNLOAD);
        prop_write5(a, 4, START);
        alloc_obj(a, 4, sizeof SEG, 1, 0);
        mem_write(a, SEG_BASE + 3 * SEG_SIZE, SEG, sizeof SEG);
        set_app(a, 0xC001);
        prop_write5(a, 4, DONE);
        g_tchk_err = 0;
        ck(s_d[a].lo[4].state == LS_ERROR && s_d[a].lo[4].err == KNX_ERR_NO_RAM && sent(0, A, "03d60405100103"),
           "check refuses at LoadCompleted: the answer to the load event says Error");
        ck(g_note_n[GW_NOTE_REFUSED] == ref0 + 1 && g_note_kind == GW_NOTE_REFUSED && g_note_err == KNX_ERR_NO_RAM &&
           g_note_ia == 0xFF01, "the gateway notes the refusal with address and code");
        ck(s_d[a].k == -1 && g_zlink_del == del0 + 1, "an application in Error does not run: the class is unlinked");
        pread(a, 4, 28, 1, 1);
        ck(sent(0, A, "03d6041c100107"), "PID 28 reads 7, insufficient volatile memory");
        pread(a, 4, 6, 1, 1);
        ck(sent(0, A, "03d60406100100"), "run state reads Halted");
        pread(a, 3, 28, 1, 1);
        ck(sent(0, A, "03d6031c100100"), "PID 28 of an object without error reads 0");
        uint8_t pd[5] = {0x03, 0xD8, 4, 28, 0};
        feed(A, 0xFF01, false, false, pd, 5);
        ck(g_last_tx_n == 1 && g_last_tx[0].len == 9 && g_last_tx[0].tpdu[3] == 28 && g_last_tx[0].tpdu[5] == 0x02,
           "PID 28 is described, unsigned char");
        prop_write5(a, 4, DONE);
        ck(s_d[a].lo[4].state == LS_ERROR && s_d[a].lo[4].err == KNX_ERR_NO_RAM, "LoadCompleted again: Error, first code kept");
        reboot_clean();
        ck(s_d[a].lo[4].state == LS_ERROR && s_d[a].lo[4].err == KNX_ERR_NO_RAM, "state and code survive a restart");
        prop_write5(a, 4, UNLOAD);
        ck(s_d[a].lo[4].state == LS_UNLOADED && s_d[a].lo[4].err == 0, "Unload: state Unloaded, code 0");
        load4(a, SEG, sizeof SEG);
        ck(s_d[a].lo[4].state == LS_LOADED && s_d[a].lo[4].err == 0 && sent(0, A, "03d60405100101"),
           "a download the check passes: Loaded");
        // what the device info of ETS shows (seen 2026-11-13): order info and hardware type of the application that
        // failed, PEI program halted while unloaded, no bus voltage
        prop_write5(a, 4, UNLOAD);
        prop_write5(a, 4, START);
        alloc_obj(a, 4, sizeof SEG, 1, 0);
        set_app(a, 0xC003);
        g_tchk_err = KNX_ERR_NO_RAM;
        prop_write5(a, 4, DONE);
        g_tchk_err = 0;
        pread(a, 0, 15, 1, 1);
        ck(g_last_tx_n == 1 && g_last_tx[0].len == 16 && memcmp(g_last_tx[0].tpdu + 6, "ZB-SWITCH ", 10) == 0,
           "order info of a C003 in Error: ZB-SWITCH");
        pread(a, 0, 78, 1, 1);
        ck(sent(0, A, "03d6004e10010000c0030100"), "hardware type of a C003 in Error: 0000 C003 01 00");
        pread(a, 5, 6, 1, 1);
        ck(sent(0, A, "03d60506100100"), "PEI program not loaded: run state Halted");
        uint8_t adc[3] = {0x01, 0x81, 0x08};
        feed(A, 0xFF01, false, false, adc, 3);
        ck(sent(0, A, "01c1000000"), "ADC channel 1: read count 0, nothing to measure");
        // the test hook of the console: once
        vdev_test_refuse(KNX_ERR_CRC);
        load4(a, SEG, sizeof SEG);
        ck(s_d[a].lo[4].state == LS_ERROR && s_d[a].lo[4].err == KNX_ERR_CRC, "tplfail 9: the next download ends in Error 9");
        load4(a, SEG, sizeof SEG);
        ck(s_d[a].lo[4].state == LS_LOADED, "tplfail works once");
        // applications that do not belong here
        prop_write5(a, 4, UNLOAD);
        prop_write5(a, 4, START);
        alloc_obj(a, 4, sizeof SEG, 1, 0);
        set_app(a, 0xC000);
        prop_write5(a, 4, DONE);
        ck(s_d[a].lo[4].state == LS_ERROR && s_d[a].lo[4].err == KNX_ERR_CONFIG, "the gateway's application in a device: Error 3");
        prop_write5(a, 4, UNLOAD);
        prop_write5(a, 4, START);
        alloc_obj(a, 4, sizeof SEG, 1, 0);
        set_app(a, 0xC0FF);
        prop_write5(a, 4, DONE);
        ck(s_d[a].lo[4].state == LS_ERROR && s_d[a].lo[4].err == KNX_ERR_CONFIG,
           "an application without template that is no class of the firmware: Error 3");
        g_tpl_mode = 1;
        prop_write5(a, 4, UNLOAD);
        prop_write5(a, 4, START);
        alloc_obj(a, 4, sizeof SEG, 1, 0);
        set_app(a, 0xC0FF);
        prop_write5(a, 4, DONE);
        ck(s_d[a].lo[4].state == LS_LOADED && s_d[a].next_tpl, "the same application with a template: Loaded");
        g_tpl_mode = 0;
        // an allocation the gateway does not take
        prop_write5(a, 4, UNLOAD);
        prop_write5(a, 4, START);
        alloc_obj(a, 4, 0, 0, 0);
        ck(s_d[a].lo[4].err == KNX_ERR_ALLOC_0, "allocation of size 0: code 8");
        prop_write5(a, 4, UNLOAD);
        prop_write5(a, 4, START);
        alloc_obj(a, 4, 16385, 0, 0);
        ck(s_d[a].lo[4].err == KNX_ERR_TOO_LONG, "allocation above 16 KiB: code 13");
        prop_write5(a, 1, UNLOAD);
        alloc_obj(a, 1, 10, 0, 0);
        ck(s_d[a].lo[1].state == LS_ERROR && s_d[a].lo[1].err == KNX_ERR_LOAD_CMD, "allocation outside Loading: code 14");
        // a template that passes the check and does not come up after the restart: Error until the next download
        prop_write5(a, 1, UNLOAD);
        g_tpl_mode = 2;
        load4(a, SEG, sizeof SEG);
        ck(s_d[a].lo[4].state == LS_LOADED && !s_d[a].fault, "mode 2 passes the check: Loaded");
        unsigned long f0 = g_note_n[GW_NOTE_FAULT];
        uint8_t rst[2] = {0x03, 0x80};
        feed(A, 0xFF01, false, false, rst, 2);
        ck(s_d[a].fault == KNX_ERR_CONFIG && s_d[a].k == -1 && g_note_n[GW_NOTE_FAULT] == f0 + 1,
           "refused at the restart: fault 3, no class, the gateway notes it");
        pread(a, 4, 5, 1, 1);
        ck(sent(0, A, "03d60405100103"), "load state reads Error while it does not run");
        pread(a, 4, 28, 1, 1);
        ck(sent(0, A, "03d6041c100103"), "PID 28 reads the fault");
        pread(a, 4, 6, 1, 1);
        ck(sent(0, A, "03d60406100100"), "run state reads Halted");
        reboot_clean();
        ck(s_d[a].lo[4].state == LS_LOADED && !s_d[a].fault, "the fault is not stored: after a restart it is tried again");
        g_tpl_mode = 0;
        // the gateway: only its own application
        init_dev(&s_d[GW]);
        s_d[GW].ia = 0xFF30;
        publish();
        prop_write5(GW, 4, UNLOAD);
        prop_write5(GW, 4, START);
        alloc_obj(GW, 4, 4, 1, 1);
        set_app(GW, 0xC001);
        prop_write5(GW, 4, DONE);
        ck(s_d[GW].lo[4].state == LS_ERROR && s_d[GW].lo[4].err == KNX_ERR_CONFIG, "a Zigbee device's application in the gateway: Error 3");
        prop_write5(GW, 4, UNLOAD);
        prop_write5(GW, 4, START);
        alloc_obj(GW, 4, 4, 1, 1);
        set_app(GW, 0xC000);
        prop_write5(GW, 4, DONE);
        ck(s_d[GW].lo[4].state == LS_LOADED, "its own application: Loaded");
    }
    // the gateway's record moves from "v8" (8 slots in earlier firmware) to "vgw", once
    fresh();
    {
        static const uint8_t OLD[5] = {'V', 'D', 'V', '2', 0x77};
        nvs_set_blob(1, "v8", OLD, 5);
        uint8_t got[8] = {0};
        size_t gn = sizeof got, ln = 0, vn = 0;
        bool r = gw_key_move();
        ck(r && nvs_get_blob(1, "vgw", got, &gn) == ESP_OK && gn == 5 && memcmp(got, OLD, 5) == 0 &&
           nvs_get_blob(1, "v8", NULL, &vn) != ESP_OK && nvs_get_blob(1, "layout", NULL, &ln) == ESP_OK,
           "first start with 32 slots: v8 becomes vgw, layout set");
        nvs_set_blob(1, "v8", OLD, 4);   // now virtual device 8's own record
        ck(gw_key_move() && nvs_get_blob(1, "v8", NULL, &vn) == ESP_OK && vn == 4, "with layout: v8 stays device 8's");
        nvs_erase_key(1, "layout");
        static const uint8_t NEWER[3] = {9, 9, 9};
        nvs_set_blob(1, "vgw", NEWER, 3);
        nvs_set_blob(1, "v8", OLD, 5);
        gn = sizeof got;
        ck(gw_key_move() && nvs_get_blob(1, "v8", NULL, &vn) != ESP_OK && nvs_get_blob(1, "vgw", got, &gn) == ESP_OK &&
           gn == 3, "restart between the steps: vgw is kept, v8 goes");
    }
    // long frames: PID 56 decides what a client may send; memory services up to 63 octets in one extended frame
    fresh();
    {
        int e = vdev_add(0xFFFF, 1, 0, NULL);
        vdev_set_ia(e, IA);
        s_d[e].device_control = 0x04;   // verify mode, as ETS sets it
        pread(e, 0, 56, 1, 1);
        ck(g_last_tx_n == 1 && g_last_tx[0].len == 8 && g_last_tx[0].tpdu[6] == 0 && g_last_tx[0].tpdu[7] == 15,
           "PID 56 is 15 by default");
        prop_write5(e, 1, START);
        alloc_obj(e, 1, 200, 1, 0x00);
        uint8_t t[KNX_TPDU_MAX], want[2 * KNX_TPDU_MAX + 1];
        t[0] = 0x02; t[1] = 0x80 | 40; t[2] = 0x40; t[3] = 0x10;   // A_Memory_Write, 40 octets at 4010h
        for (int i = 0; i < 40; i++) t[4 + i] = (uint8_t)(0xA0 + i);
        feed(A, IA, false, false, t, 44);
        bool in = true;
        for (int i = 0; i < 40; i++) in = in && s_d[e].lo[1].seg[0x10 + i] == (uint8_t)(0xA0 + i);
        snprintf((char *)want, sizeof want, "0268%s", "4010");
        for (int i = 0; i < 40; i++) sprintf((char *)want + 8 + 2 * i, "%02x", 0xA0 + i);
        ck(in && g_last_tx_n == 1 && g_last_tx[0].len == 44 && sent(0, A, (char *)want),
           "memory write of 40 octets (extended frame): written, echo of 44 octets with the data");
        uint8_t r[4] = {0x02, 40, 0x40, 0x10};   // A_Memory_Read, 40 octets
        feed(A, IA, false, false, r, 4);
        ck(g_last_tx_n == 1 && sent(0, A, "02404010"), "memory read of 40 octets with PID 56 = 15: count 0");
        ck(vdev_max_apdu(66) == 66 && vdev_max_apdu(67) == 66 && vdev_max_apdu(14) == 66 && vdev_max_apdu(0) == 66,
           "apdu 66 taken, 67 and 14 refused, 0 reads");
        pread(e, 0, 56, 1, 1);
        ck(g_last_tx_n == 1 && g_last_tx[0].tpdu[7] == 66, "PID 56 reads 66 then");
        feed(A, IA, false, false, r, 4);
        ck(g_last_tx_n == 1 && g_last_tx[0].len == 44 && sent(0, A, (char *)want), "memory read of 40 octets with PID 56 = 66");
        uint8_t r63[4] = {0x02, 63, 0x40, 0x00};
        feed(A, IA, false, false, r63, 4);
        ck(g_last_tx_n == 1 && g_last_tx[0].len == KNX_TPDU_MAX && g_last_tx[0].tpdu[1] == (0x40 | 63),
           "memory read of 63 octets: an answer of 67 octets");
        t[1] = 0x80 | 63; t[2] = 0x40; t[3] = 0x80;   // 63 octets at 4080h
        for (int i = 0; i < 63; i++) t[4 + i] = (uint8_t)i;
        feed(A, IA, false, false, t, KNX_TPDU_MAX);
        ck(g_last_tx_n == 1 && g_last_tx[0].len == KNX_TPDU_MAX && s_d[e].lo[1].seg[0x80 + 62] == 62,
           "memory write of 63 octets: written, echo of 67 octets");
        // connected: the answer of 67 octets goes through the connection queue
        conn(A, IA, 0x80);
        data(A, IA, 0, r63, 4);
        ck(g_last_tx_n == 2 && sent(0, A, "c2") && g_last_tx[1].len == KNX_TPDU_MAX && g_last_tx[1].tpdu[0] == 0x42 &&
           s_d[e].c.qn[s_d[e].c.qh] == KNX_TPDU_MAX, "connected memory read of 63 octets: T_ACK, then 67 octets with seq 0");
        conn(A, IA, 0xC2);
        conn(A, IA, 0x81);
        ck(vdev_max_apdu(15) == 15, "back to 15");
    }
    // availability: the capability of the device announce is kept, the link learns whether it sleeps
    fresh();
    {
        uint8_t ie[8] = {0x02, 0x00, 0x00, 0xff, 0xfe, 1, 2, 3}, other[8] = {9, 9, 9, 9, 9, 9, 9, 9};
        int z = vdev_add(0x1234, 1, -1, ie);
        vdev_set_ia(z, 0xFF05);
        g_sleepy_calls = 0;
        vdev_set_capability(ie, 0x80);   // allocate address, receiver off when idle: a sleepy end device
        ck(s_d[z].cap_known && !s_d[z].rx_on && g_sleepy_calls == 1 && g_sleepy_last == 1 && s_d[z].dirty,
           "announce without 'receiver on when idle': sleepy, the link is told, the record is to be saved");
        vdev_set_capability(ie, 0x8e);   // the LED1934G3: FFD, mains, receiver on
        ck(s_d[z].rx_on && g_sleepy_calls == 2 && g_sleepy_last == 0, "announce with 'receiver on': not sleepy");
        reboot_clean();
        ck(s_d[z].used && s_d[z].cap_known && s_d[z].rx_on, "the capability survives a restart (record flags 4, 8)");
        vdev_set_capability(ie, 0x80);
        reboot_clean();
        ck(s_d[z].cap_known && !s_d[z].rx_on, "... also 'sleepy'");
        g_sleepy_calls = 0;
        vdev_set_capability(other, 0x80);
        ck(g_sleepy_calls == 0, "an IEEE address none of ours has: nothing");
        static const uint8_t P10[10] = {1, 1, 100, 99, 1, 1, 100, 0, 0, 60};
        load4(z, P10, 10);                       // C001 without a template: the built-in class
        g_sleepy_calls = 0;
        feed(A, 0xFF05, false, false, RST, 2);   // restart: apply() links the class
        ck(g_sleepy_calls == 1 && g_sleepy_last == 1, "linking after a download tells the link it sleeps");
        g_note_kind = 0;
        vdev_avail_note(s_d[z].inst, false);
        ck(g_note_kind == GW_NOTE_OFFLINE && g_note_ia == 0xFF05, "device off: message with its address");
        vdev_avail_note(s_d[z].inst, true);
        ck(g_note_kind == GW_NOTE_ONLINE && g_note_ia == 0xFF05, "device back: message with its address");
        g_note_kind = 0;
        vdev_avail_note(999, false);
        ck(g_note_kind == 0, "an instance none of ours has: no message");
        // the full interview: the function is on another endpoint
        g_zset_n = 0;
        tick_for(3000000);
        ck(vdev_endpoint_of(0x1234) == 1 && vdev_endpoint_of(0x4321) == 0, "endpoint of a short address: 1, none");
        g_params_n = 0;
        vdev_set_endpoint(0x1234, 11);
        ck(s_d[z].zep == 11 && s_d[z].dirty && g_zset_n == 1 && g_zset_ep == 11,
           "interview finds endpoint 11: kept, the linked class moves there");
        ck(g_params_n == 1 && strstr(g_last_params, "\"p\":\"0101646301016400003c\"") != NULL,
           "after the move the class gets the parameters of the download again, not its defaults");
        vdev_set_endpoint(0x1234, 11);
        ck(g_zset_n == 1, "the same endpoint again: nothing");
        vdev_set_endpoint(0x4321, 3);
        ck(g_zset_n == 1 && s_d[z].zep == 11, "another short address: nothing");
        reboot_clean();
        ck(s_d[z].zep == 11, "the endpoint survives a restart");
        g_sleepy_calls = 0;
        vdev_set_capability_of(0x1234, 0x80);
        ck(g_sleepy_calls == 1 && g_sleepy_last == 1 && !s_d[z].rx_on, "capability by short address (node descriptor)");
    }
    printf("behaviour checks: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail != 0;
}
