// Behaviour checks against the real vdev.c and gw.c: what the device server and the gateway's own device answer
// (host harness)
#define WITH_GW
#define WITH_CONSOLE
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
static void gw_load(const uint8_t *p, int n)   // a download of the gateway's application with these parameters
{
    prop_write5(GW, 4, UNLOAD);
    prop_write5(GW, 4, START);
    alloc_obj(GW, 4, (uint32_t)n, 1, 0);
    mem_write(GW, SEG_BASE + 3 * SEG_SIZE, p, n);
    set_app(GW, 0xC000);
    prop_write5(GW, 4, DONE);
}
static void restart_of(uint16_t ia)   // connectionless A_Restart, as ETS sends it at the end of a download
{
    static const uint8_t RS[2] = {0x03, 0x80};
    feed(PARTNER[0], ia, false, false, RS, 2);
}
static void server_start(void)   // vdev_start() once more, as after a power cycle; what the last one made goes first
{
    if (s_q) { free(s_q->buf); free(s_q); }
    if (s_kq) { free(s_kq->buf); free(s_kq); }
    free(s_mx);
    g_task_runs = false;   // until vdev_start() creates it again
    vdev_start();
}
// the gateway's tables: 12 objects; with links, object 3 on 31/6/13, 11 on 31/6/17 and 12 on 31/6/18
static void gw_tables(bool linked)
{
    uint8_t go[2 + 2 * 12] = {0, 12};
    for (int k = 1; k <= 12; k++) {
        go[2 * k] = 0x4F;   // communication, read, transmit, low priority
        go[2 * k + 1] = k == 11 ? 14 : k == 3 ? 7 : 0;   // 14 octets, 1 octet, 1 bit
    }
    static const uint8_t AT[8] = {0, 3, 0xF6, 0x0D, 0xF6, 0x11, 0xF6, 0x12},
                         AS[14] = {0, 3, 0, 1, 0, 3, 0, 2, 0, 11, 0, 3, 0, 12}, NONE[2] = {0, 0};
    for (int o = 3; o >= 1; o--) prop_write5(GW, o, UNLOAD);
    prop_write5(GW, 3, START); alloc_obj(GW, 3, sizeof go, 1, 0); mem_write(GW, 0x4800, go, sizeof go);
    prop_write5(GW, 1, START); alloc_obj(GW, 1, linked ? 8 : 2, 1, 0); mem_write(GW, 0x4000, linked ? AT : NONE, linked ? 8 : 2);
    prop_write5(GW, 2, START); alloc_obj(GW, 2, linked ? 14 : 2, 1, 0); mem_write(GW, 0x4400, linked ? AS : NONE, linked ? 14 : 2);
    for (int o = 3; o >= 1; o--) prop_write5(GW, o, DONE);
}
static bool gw_sent(uint16_t ga, const char *hexs)   // one of the frames since g_last_tx_n was cleared
{
    char h[2 * KNX_TPDU_MAX + 1];
    for (int i = 0; i < g_last_tx_n; i++) {
        for (int k = 0; k < g_last_tx[i].len; k++) sprintf(h + 2 * k, "%02x", g_last_tx[i].tpdu[k]);
        if (g_last_tx[i].group && g_last_tx[i].dst == ga && strcmp(h, hexs) == 0) return true;
    }
    return false;
}
static const char *note_hex(const char *txt)   // GroupValue_Write of a DPT 16.001 text
{
    static char h[40];
    uint8_t v[14] = {0};
    memcpy(v, txt, strlen(txt));
    sprintf(h, "0080");
    for (int i = 0; i < 14; i++) sprintf(h + 4 + 2 * i, "%02x", v[i]);
    return h;
}
static void reboot_clean(void)   // saved as timers() would, RAM gone, the records back as vdev_start() reads them
{
    for (int i = 0; i < NDEV; i++) if (s_d[i].used) save(&s_d[i]);
    wipe_ram();
    for (int i = 0; i < VDEV_MAX; i++) restore(i);
    if (!restore(GW)) init_dev(&s_d[GW]);
    ts_sweep();
    publish();
}
static int nvs_tparts(void)
{
    int n = 0;
    for (int k = 0; k < NKEY; k++) n += g_nvs[k].b && g_nvs[k].key[0] == 't';
    return n;
}
// a parameter segment with a template part of about 3.3 KB (text v), as a product with a large template brings it
static int big_seg(int v, uint8_t *out)
{
    static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int p = 10, l = 3300;
    for (int i = 0; i < p; i++) out[i] = (uint8_t)(i + v);
    for (int i = 0; i < l; i++) out[p + i] = (uint8_t)B64[(i * (v + 5) + v) % 64];
    char tr[17];
    snprintf(tr, sizeof tr, "KGTE%04X%04X%04X", (unsigned)p, (unsigned)l, (unsigned)p);
    memcpy(out + p + l, tr, 16);
    return p + l + 16;
}
static bool nvs_has_part(const uint8_t *seg, int n)   // the blob of this segment's template part is in the flash
{
    uint32_t at = tpl_split(seg, (uint32_t)n);
    char k[16];
    ts_key(crc32_of(seg + at, (uint32_t)n - at), (uint32_t)n - at, k);
    return at && nvs_find(k) >= 0;
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
    // a template part leaves the flash as soon as no stored record names it (before, it stayed until the next start),
    // and a blob that does not fit is not written at all
    fresh();
    {
        static uint8_t X[3400], Y[3400], Z[3400], back[3400];
        int nx = big_seg(1, X), ny = big_seg(2, Y), nz = big_seg(3, Z);
        int a = vdev_add(0xFFFF, 1, 0, NULL), b = vdev_add(0xFFFF, 1, 0, NULL);
        vdev_set_ia(a, 0xFF01);
        vdev_set_ia(b, 0xFF02);
        load4(a, X, nx);
        save(&s_d[a]);
        save(&s_d[b]);
        ck(nvs_tparts() == 1 && nvs_has_part(X, nx), "device a with template X: its part in the flash");
        load4(a, Y, ny);
        save(&s_d[a]);
        ck(nvs_tparts() == 1 && nvs_has_part(Y, ny) && !nvs_has_part(X, nx),
           "a changes to Y: X's part goes when a's record names Y, not at the next start");
        load4(b, Y, ny);
        save(&s_d[b]);
        load4(a, Z, nz);
        save(&s_d[a]);
        ck(nvs_tparts() == 2 && nvs_has_part(Y, ny) && nvs_has_part(Z, nz), "a changes to Z: Y stays, b's record names it");
        vdev_del(b);
        ck(nvs_tparts() == 1 && !nvs_has_part(Y, ny), "b deleted: Y's part goes with b's record");
        reboot_clean();
        seg_read(&s_d[a].lo[4], 0, back, (uint32_t)nz);
        ck(s_d[a].lo[4].state == LS_LOADED && memcmp(back, Z, (size_t)nz) == 0, "after a restart a comes back with Z");
        // a partition with room for one part more than it holds, not two (the TULX32's actor_db with 32 devices)
        g_nvs_cap = nvs_used() + 126 + 150;
        bool all = true;
        for (int r = 0; r < 6; r++) {
            const uint8_t *s = r % 3 == 0 ? X : r % 3 == 1 ? Y : Z;
            load4(a, s, nx);
            save(&s_d[a]);
            all = all && !s_d[a].dirty && nvs_tparts() == 1;
        }
        ck(all, "six template changes in a row on a nearly full partition: every one saved");
        // no room even for one part: NVS is not asked to write what it cannot take
        g_nvs_cap = nvs_used() + 126 + 20;
        unsigned long ns0 = g_nvs_nospace, er0 = s_save_errs;
        load4(a, Y, ny);
        save(&s_d[a]);
        ck(s_d[a].dirty && s_save_errs == er0 + 1 && g_nvs_nospace == ns0,
           "no room for the new part: the save fails before NVS writes anything");
        g_nvs_cap = nvs_used() + 126;   // every usable entry taken
        ck(vdev_store_pct() == 100, "storage in use: 100 % when nothing is left (the page NVS keeps does not count)");
        g_nvs_cap = 0;
    }
    // a write the storage refuses is told on the gateway's message object, the fault stays on while the record waits,
    // and it is tried again less and less often
    fresh();
    {
        int d = vdev_add(0xFFFF, 1, 0, NULL);
        vdev_set_ia(d, 0xFF01);
        tick_for(3000000);
        unsigned long n0 = g_note_n[GW_NOTE_STORE], sets0 = g_nvs_sets;
        ck(!s_d[d].dirty && !vdev_store_failing() && fault_now(vdev_free_slots()) == 0, "saved, no fault");
        g_nvs_fail = 1;
        vdev_set_ia(d, 0xFF03);
        tick_for(3000000);
        ck(s_d[d].dirty && g_note_n[GW_NOTE_STORE] == n0 + 1 && g_note_ia == 0xFF03 && vdev_store_failing() &&
           fault_now(vdev_free_slots()) == 1, "the write fails: a message with the device's address, the gateway's fault on");
        tick_for(3600 * 1000000LL);
        unsigned long tries = g_nvs_sets - sets0;
        ck(tries <= 12 && g_note_n[GW_NOTE_STORE] == n0 + 1,
           "an hour of a refusing storage: at most 12 attempts (360 before), and one message for the series");
        g_nvs_fail = 0;
        tick_for(700 * 1000000LL);
        ck(!s_d[d].dirty && !vdev_store_failing() && fault_now(vdev_free_slots()) == 0,
           "the storage takes it again: saved at the next attempt, the fault goes");
        // at the start: a partition NVS cannot use is erased with a log line and a message; none at all is a fault
        fresh();
        unsigned long e0 = g_log['E'];
        int er0 = g_nvs_erased;
        n0 = g_note_n[GW_NOTE_STORE];
        g_nvs_init_err = ESP_ERR_NVS_NO_FREE_PAGES;
        server_start();
        ck(g_nvs_erased == er0 + 1 && g_log['E'] > e0 && g_note_n[GW_NOTE_STORE] == n0 + 1 && g_note_ia == 0xFFFF &&
           s_nvs_ok && !vdev_store_failing(), "partition erased at the start: a log line and a message, then it works");
        g_nvs_open_err = ESP_FAIL;
        server_start();
        ck(!s_nvs_ok && vdev_store_failing() && fault_now(vdev_free_slots()) == 1 && g_note_n[GW_NOTE_STORE] == n0 + 2,
           "no storage at all: a message, and the gateway's fault stays on");
        fresh();
        server_start();
    }
    // the gateway's own objects: fault and message reach the bus after the start and after every restart of its device
    // (also when they came while the objects had no group address), every message goes out, and nothing on the way
    // into its objects is lost
    fresh();
    {
        init_dev(&s_d[GW]);
        s_d[GW].ia = 0xFF30;
        publish();
        static const uint8_t P[5] = {1, 1, 47, 180, 0};
        gw_tables(false);
        gw_load(P, 5);
        restart_of(0xFF30);
        g_zl_unavailable = 1;   // a Zigbee device that does not answer: fault 1
        gw_note(GW_NOTE_FAULT, 0xFF05, KNX_ERR_SOFTWARE, "a template that did not come up at the start");
        g_last_tx_n = 0;
        status_round(true);
        ck(g_last_tx_n == 0 && s_d[GW].ko[12].have && s_d[GW].ko[12].v[0] == 1,
           "first round, objects without group addresses: fault 1 is only in the object");
        gw_tables(true);        // ETS links objects 3, 11 and 12 ...
        restart_of(0xFF30);     // ... and restarts the gateway
        g_last_tx_n = 0;
        status_round(false);
        ck(gw_sent(0xF612, "0081") && gw_sent(0xF611, note_hex("15.15.5 SW")),
           "after the gateway's restart: fault 1 and the last message go out");
        g_last_tx_n = 0;
        status_round(false);
        ck(g_last_tx_n == 0, "the round after: nothing new, nothing sent");
        gw_note(GW_NOTE_OFFLINE, 0xFF02, 0, "x");
        gw_note(GW_NOTE_OFFLINE, 0xFF03, 0, "x");
        gw_note(GW_NOTE_OFFLINE, 0xFF03, 0, "x");
        g_last_tx_n = 0;
        status_round(false);
        bool order = g_last_tx_n == 3;
        for (int i = 0; i < 3 && order; i++) {
            const char *want = i ? "15.15.3 OFF" : "15.15.2 OFF";
            order = g_last_tx[i].dst == 0xF611 && memcmp(g_last_tx[i].tpdu + 2, want, strlen(want)) == 0;
        }
        ck(order, "three messages in one round, the last two alike: three telegrams, in order");
        // a start with the fault present: its first round sends fault and message (the objects have their addresses)
        s_told_n = s_tell_n - 1;
        s_fault_n = -1;
        g_last_tx_n = 0;
        status_round(true);
        ck(gw_sent(0xF612, "0081") && gw_sent(0xF611, note_hex("15.15.3 OFF")) && !gw_sent(0xF60D, "0080"),
           "first round after a start: fault and last message go out, the other status objects are only kept");
        // the device server's frame queue is full: what the gateway puts still reaches its objects
        ev_t junk = {.type = EV_FRAME};
        while (xQueueSend(s_q, &junk, 0) == pdTRUE) {}
        g_zl_unavailable = 0;
        g_last_tx_n = 0;
        status_round(false);
        ck(s_d[GW].ko[12].v[0] == 0 && gw_sent(0xF612, "0080"), "fault 0 with a full queue: in the object and on the bus");
        while (xQueueReceive(s_q, &junk, 0) == pdTRUE) {}
        char longwhy[121];
        memset(longwhy, 'x', 120);
        longwhy[120] = 0;
        gw_note(GW_NOTE_REFUSED, 0xFF05, KNX_ERR_CONFIG, longwhy);
        ck(strlen(s_note.why) == 120, "a reason of 120 characters is kept whole for the console");
        status_round(false);
    }
    // the device server's queues. A central command to 32 lamps: each answers with two status values while the device
    // server is still busy with the command (on the stick 8-9 ms per device, the lamp's status about 100 ms after its
    // command). And a start with 32 devices whose classes send their first values while the next device comes back.
    fresh();
    {
        for (int i = 0; i < VDEV_MAX; i++) {
            int n = vdev_add((uint16_t)(0x1000 + i), 1, 0, NULL);   // runs built-in class 0
            vdev_set_ia(n, (uint16_t)(0xFF01 + n));
            load_light(n);
        }
        unsigned long l0 = s_ev_lost + s_kv_lost, kw0 = g_tx_n;
        s_q_peak = s_kq_peak = 0;
        g_kw_values = 2;
        uint8_t wr[2] = {0x00, 0x80};
        feed(0x1105, 0xF601, true, false, wr, 2);   // central off on 31/6/1, which every lamp has on its KO 1
        g_kw_values = 0;
        ck(s_ev_lost + s_kv_lost == l0 && s_kq_peak == 64,
           "central command to 32 lamps, two status values each meanwhile: 64 waited, none lost");
        (void)kw0;
        for (int i = 0; i < VDEV_MAX; i++) save(&s_d[i]);
        wipe_ram();
        g_set_values = 3;
        s_kq_peak = 0;
        l0 = s_ev_lost + s_kv_lost;
        server_start();
        g_set_values = 0;
        ck(s_ev_lost + s_kv_lost == l0 && s_kq_peak <= 3 && s_d[VDEV_MAX - 1].k == 0,
           "start of 32 devices with three first values each: taken device by device, none lost");
    }
    // a restart does not check a template again whose VM runs (a partial download of the tables included); a check
    // that finds no memory is tried again instead of leaving the device without its class until the next restart
    fresh();
    {
        uint8_t s0[TPL_SEG_MAX], s1[TPL_SEG_MAX];
        int n0 = tpl_seg(0, s0), n1 = tpl_seg(1, s1);
        int a = vdev_add(0x1234, 1, 0, NULL);
        vdev_set_ia(a, 0xFF01);
        g_tpl_mode = 1;
        load4(a, s0, n0);
        restart_of(0xFF01);
        unsigned long c0 = g_tpl_calls;
        ck(s_d[a].k == 2 && !s_d[a].fault, "template loaded and running");
        restart_of(0xFF01);
        load_light(a);              // a partial download: the tables anew, object 4 unloaded and loaded again ...
        prop_write5(a, 4, START);
        alloc_obj(a, 4, (uint32_t)n0, 0, 0);   // ... without fill: it keeps its content
        prop_write5(a, 4, DONE);
        restart_of(0xFF01);
        ck(g_tpl_calls == c0 && s_d[a].k == 2, "two restarts, one after a partial download of the tables: not checked again");
        int b = vdev_add(0x2345, 1, 0, NULL);
        vdev_set_ia(b, 0xFF02);
        load4(b, s0, n0);
        restart_of(0xFF02);
        ck(g_tpl_calls == c0 && s_d[b].k == 2, "a second device with the same template, checked at LoadCompleted: not checked again");
        g_tpl_mode = 3;   // no memory for the check of the next template
        load4(a, s1, n1);
        restart_of(0xFF01);
        ck(s_d[a].fault == KNX_ERR_NO_RAM && s_d[a].k == -1, "a new template and no memory to check it: no class, Error 7");
        unsigned long f0 = g_note_n[GW_NOTE_FAULT];
        tick_for(31000000);
        ck(s_d[a].fault == KNX_ERR_NO_RAM && g_note_n[GW_NOTE_FAULT] == f0, "tried again after 30 s, still no memory: no new message");
        g_tpl_mode = 1;
        tick_for(31000000);
        ck(!s_d[a].fault && s_d[a].k == 2, "memory there again: the next attempt brings the device up");
        g_tpl_mode = 0;
    }
    // a device without a running class answers no read with what the class said before, nor does an object under
    // another application; unloading the application stops the class when the connection ends
    fresh();
    {
        static const uint8_t SEG[12] = {1, 1, 100, 99, 1, 1, 100, 0, 0, 60, 0, 0};
        uint8_t one = 1, rd[2] = {0, 0};
        int a = vdev_add(0x1234, 1, 0, NULL);
        vdev_set_ia(a, 0xFF01);
        load_light(a);
        load4(a, SEG, sizeof SEG);
        restart_of(0xFF01);
        vdev_ko_out(100 + a, 4, &one, 1, false, true); pump();
        feed(0x1105, 0xF604, true, false, rd, 2);
        ck(s_d[a].k == 0 && g_last_tx_n == 1 && g_last_tx[0].tpdu[1] == 0x41, "class runs: a read of the status gets 1");
        g_tpl_mode = 2;
        restart_of(0xFF01);   // its template does not come up now: no class
        g_tpl_mode = 0;
        feed(0x1105, 0xF604, true, false, rd, 2);
        ck(s_d[a].k == -1 && g_last_tx_n == 0, "no class runs: the read gets no answer (it got the old 1 before)");
        restart_of(0xFF01);
        vdev_ko_out(100 + a, 4, &one, 1, false, true); pump();
        set_app(a, 0xC001);   // ETS writes the same application again
        feed(0x1105, 0xF604, true, false, rd, 2);
        ck(g_last_tx_n == 1, "the same application written again: the value stays");
        set_app(a, 0xC002);   // another one
        feed(0x1105, 0xF604, true, false, rd, 2);
        ck(g_last_tx_n == 0, "another application: what the object held under the old one is not answered");
        load4(a, SEG, sizeof SEG);
        restart_of(0xFF01);
        ck(s_d[a].k == 0, "C001 again: the class runs");
        conn(A, 0xFF01, 0x80);
        prop_write5(a, 4, UNLOAD);
        ck(s_d[a].k == 0, "application unloaded, the connection still open: the class still runs");
        conn(A, 0xFF01, 0x81);
        ck(s_d[a].k == -1, "the connection ends with the application unloaded: the class stops (it ran on before)");
        conn(A, 0xFF01, 0x80);   // a whole download in one connection, ended by its restart
        load4(a, SEG, sizeof SEG);
        uint8_t rs[2] = {0x03, 0x80};
        data(A, 0xFF01, 0, rs, 2);
        ck(s_d[a].k == 0, "unloaded and loaded again in one connection, then the restart: the class runs");
        prop_write5(a, 4, UNLOAD);
        restart_of(0xFF01);
        ck(s_d[a].k == -1, "unloaded without a connection, then a restart: the class stops");
    }
    // A_Restart: a master reset is answered (only the confirmed restart is carried out); responses and reserved bits
    // are ignored. Answers of device management go with priority low, the transport layer's control with system.
    fresh();
    {
        int a = vdev_add(0xFFFF, 1, 0, NULL);
        vdev_set_ia(a, 0xFF01);
        uint8_t mr[4] = {0x03, 0x81, 0x01, 0x00};   // master reset, erase code 01h (confirmed restart), channel 0
        vdev_prog(a, true);
        feed(A, 0xFF01, false, false, mr, 4);
        ck(sent(0, A, "03a1000000") && !s_d[a].prog, "confirmed restart: answered with error code 0, then restarted");
        mr[2] = 0x02;   // factory reset
        vdev_prog(a, true);
        feed(A, 0xFF01, false, false, mr, 4);
        ck(sent(0, A, "03a1020000") && s_d[a].prog, "factory reset: unsupported erase code, nothing restarted");
        mr[2] = 0x01;
        mr[3] = 0x01;
        feed(A, 0xFF01, false, false, mr, 4);
        ck(sent(0, A, "03a1030000") && s_d[a].prog, "confirmed restart for channel 1: invalid channel number");
        uint8_t rsv[2] = {0x03, 0x82}, rsp[5] = {0x03, 0xA1, 0x00, 0x00, 0x00};
        feed(A, 0xFF01, false, false, rsv, 2);
        ck(g_last_tx_n == 0 && s_d[a].prog, "a reserved bit set: ignored");
        feed(A, 0xFF01, false, false, rsp, 5);
        ck(g_last_tx_n == 0 && s_d[a].prog, "an A_Restart_Response from elsewhere: no restart (it restarted before)");
        static const uint8_t DD[2] = {0x03, 0x00};
        feed(A, 0xFF01, false, false, DD, 2);
        ck(g_last_tx_n == 1 && g_last_tx[0].prio == 3, "connectionless answer: priority low");
        conn(A, 0xFF01, 0x80);
        data(A, 0xFF01, 0, DD, 2);
        ck(g_last_tx_n == 2 && g_last_tx[0].prio == 0 && g_last_tx[1].prio == 3,
           "connected: the T_ACK with priority system, the answer with low");
        conn(A, 0xFF01, 0xC2);
        conn(A, 0xFF01, 0x81);
        uint8_t ar[2] = {0x01, 0x00};
        feed(A, 0, true, false, ar, 2);
        ck(g_last_tx_n == 1 && g_last_tx[0].prio == 0, "A_IndividualAddress_Response: priority system");
        vdev_prog(a, false);
    }
    // an object in Error gives its RAM back after a minute without a connection; an answer to a read the link had no
    // room for is sent as soon as there is room (it was dropped)
    fresh();
    {
        int a = vdev_add(0xFFFF, 1, 0, NULL);
        vdev_set_ia(a, 0xFF01);
        prop_write5(a, 1, START);
        alloc_obj(a, 1, 1000, 1, 0x11);
        alloc_obj(a, 1, 0, 0, 0);   // size 0: Error 8
        uint32_t held = seg_total();
        ck(s_d[a].lo[1].state == LS_ERROR && s_d[a].lo[1].cap == 1000, "object 1 in Error, its 1000 B still held");
        tick_for(61000000);
        ck(s_d[a].lo[1].state == LS_ERROR && s_d[a].lo[1].cap == 0 && !s_d[a].lo[1].seg && seg_total() == held - 1000,
           "a minute later, no connection: the 1000 B are back, the state stays Error");
        pread(a, 1, 27, 1, 1);
        ck(sent(0, A, "03d6011b10010000000000ffffff"), "PID 27 in Error then: size 0, CRC FFFF");
        prop_write5(a, 1, UNLOAD);
        load_light(a);
        uint8_t one = 1, rd[2] = {0, 0};
        vdev_ko_out(100 + a, 4, &one, 1, true, true); pump();
        unsigned long gt = s_d[a].grp_tx;
        g_txfail_pct = 100;
        feed(0x1105, 0xF604, true, false, rd, 2);
        g_txfail_pct = 0;
        ck(s_d[a].grp_tx == gt, "a read while the link has no room: nothing sent, nothing counted");
        g_last_tx_n = 0;
        tick_for(200000);
        ck(g_last_tx_n == 1 && g_last_tx[0].dst == 0xF604 && g_last_tx[0].tpdu[1] == 0x41 && s_d[a].grp_tx == gt + 1,
           "room again: the answer goes out (GroupValue_Response 1 on the address of the read)");
    }
    // a group telegram the link gave up (no confirmation of the chip) goes out again: a status at most three times,
    // an answer to a read; not a value the class sent as a command, nor one that changed since
    fresh();
    {
        int a = vdev_add(0xFFFF, 1, 0, NULL);
        vdev_set_ia(a, 0xFF01);
        load_light(a);
        uint8_t one = 1, two = 0;
        g_last_tx_n = 0;
        vdev_ko_out(100 + a, 4, &one, 1, false, true); pump();
        ck(g_last_tx_n == 1 && sent(0, 0xF604, "0081"), "status 1 sent");
        knx_frame_t f = {.src = 0xFF01, .dst = 0xF604, .group = true, .own = true, .len = 2, .tpdu = {0x00, 0x81}};
        int again = 0;
        for (int r = 0; r < 4; r++) {
            lost(&f); pump();
            g_last_tx_n = 0;
            tick_for(200000);
            again += g_last_tx_n == 1 && sent(0, 0xF604, "0081");
        }
        ck(again == 3, "the link gives it up four times: sent again three times, then left");
        vdev_ko_out(100 + a, 4, &two, 1, false, true); pump();
        lost(&f); pump();   // the link reports the old 1 lost; the object holds 0 by now
        g_last_tx_n = 0;
        tick_for(200000);
        ck(g_last_tx_n == 0, "a value that changed since: the lost one is not sent again");
        vdev_ko_out(100 + a, 4, &one, 1, false, false); pump();   // a command: always sent, not repeated
        lost(&f); pump();
        g_last_tx_n = 0;
        tick_for(200000);
        ck(g_last_tx_n == 0, "a value sent as a command: not repeated");
        vdev_ko_out(100 + a, 4, &one, 1, false, true); pump();
        knx_frame_t r = f;
        r.tpdu[1] = 0x41;   // the answer to a read
        lost(&r); pump();
        g_last_tx_n = 0;
        tick_for(200000);
        ck(g_last_tx_n == 1 && sent(0, 0xF604, "0041"), "an answer to a read the link gave up: answered again");
    }
    // the KNX serial number follows the Zigbee device: 00FA and SHA-256 over its IEEE address; the gateway's over the
    // base MAC; a device without an IEEE address keeps the earlier number from the slot
    fresh();
    {
        uint8_t h[32];
        sha256((const uint8_t *)"abc", 3, h);
        bool fips = h[0] == 0xba && h[1] == 0x78 && h[2] == 0x16 && h[3] == 0xbf && h[31] == 0xad;
        sha256((const uint8_t *)"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, h);
        fips = fips && h[0] == 0x24 && h[1] == 0x8d && h[2] == 0x6a && h[3] == 0x61 && h[31] == 0xc1;
        ck(fips, "the test's SHA-256 gives the FIPS 180-2 results");
        static const uint8_t IE1[8] = {0x78, 0xc2, 0xec, 0xfe, 0xff, 0xa7, 0xdb, 0x28}, IE2[8] = {1, 2, 3, 4, 5, 6, 7, 8},
                             MAC[6] = {0x10, 0x20, 0x30, 0x80, 0x2b, 0x7c};
        int a = vdev_add(0x1111, 1, -1, IE1), b = vdev_add(0x2222, 1, -1, NULL);
        uint8_t want[6] = {0x00, 0xFA};
        sha256(IE1, 8, h);
        memcpy(want + 2, h, 4);
        ck(memcmp(s_d[a].serial, want, 6) == 0, "device with IEEE address: 00FA and the first 4 octets of its SHA-256");
        static const uint8_t OLD_B[6] = {0x00, 0xFA, 0x80, 0x2b, 0x7c, 2};
        ck(b == 1 && memcmp(s_d[b].serial, OLD_B, 6) == 0, "device without IEEE address: base MAC and slot, as before");
        sha256(MAC, 6, h);
        memcpy(want + 2, h, 4);
        init_dev(&s_d[GW]);
        ck(memcmp(s_d[GW].serial, want, 6) == 0, "the gateway: SHA-256 over its base MAC");
        uint8_t sa[6];
        memcpy(sa, s_d[a].serial, 6);
        vdev_del(a);
        int c = vdev_add(0x3333, 1, -1, IE2);
        sha256(IE2, 8, h);
        memcpy(want + 2, h, 4);
        ck(c == a && memcmp(s_d[c].serial, want, 6) == 0 && memcmp(s_d[c].serial, sa, 6) != 0,
           "another Zigbee device in the freed slot: another serial number (it got the same one before)");
        vdev_seen(0x2222, IE1);   // b learns its IEEE address
        sha256(IE1, 8, h);
        memcpy(want + 2, h, 4);
        ck(memcmp(s_d[b].serial, want, 6) == 0, "the IEEE address learned later: the number follows it");
        memcpy(sa, s_d[c].serial, 6);
        reboot_clean();
        ck(memcmp(s_d[c].serial, sa, 6) == 0 && memcmp(s_d[b].serial, want, 6) == 0, "after a restart: the same numbers");
        uint8_t sr[8] = {0x03, 0xDC};
        memcpy(sr + 2, s_d[c].serial, 6);
        feed(A, 0, true, false, sr, 8);
        ck(g_last_tx_n == 1 && memcmp(g_last_tx[0].tpdu + 2, s_d[c].serial, 6) == 0, "A_SerialNumber_Read finds it");
    }
    // two of our devices never get the same individual address
    fresh();
    {
        int a = vdev_add(0xFFFF, 1, 0, NULL), b = vdev_add(0xFFFF, 1, 0, NULL), c = vdev_add(0xFFFF, 1, 0, NULL);
        vdev_set_ia(c, 0xFF07);
        unsigned long n0 = g_note_n[GW_NOTE_DUP];
        vdev_prog(a, true);
        vdev_prog(b, true);
        uint8_t wr[4] = {0x00, 0xC0, 0xFF, 0x06};
        feed(A, 0, true, false, wr, 4);
        ck((s_d[a].ia == 0xFF06) != (s_d[b].ia == 0xFF06) && g_note_n[GW_NOTE_DUP] == n0 + 1 && g_note_ia == 0xFF06,
           "two devices in programming mode, one address written: one takes it, the other is refused and told");
        int other = s_d[a].ia == 0xFF06 ? b : a;
        vdev_prog(a, false);
        vdev_prog(b, false);
        vdev_prog(other, true);
        wr[3] = 0x07;
        feed(A, 0, true, false, wr, 4);
        ck(s_d[other].ia == 0xFFFF && s_d[c].ia == 0xFF07, "an address another device has: not taken");
        uint8_t sw[14] = {0x03, 0xDE};
        memcpy(sw + 2, s_d[other].serial, 6);
        sw[8] = 0xFF;
        sw[9] = 0x07;
        feed(A, 0, true, false, sw, 14);
        ck(s_d[other].ia == 0xFFFF, "by serial number: not taken either");
        ck(!vdev_set_ia(other, 0xFF07) && s_d[other].ia == 0xFFFF, "nor by hand at the console");
        init_dev(&s_d[GW]);
        s_d[GW].ia = 0xFF30;
        ck(!vdev_set_ia(other, 0xFF30), "the gateway's address counts as well");
        ck(vdev_set_ia(c, 0xFFFF) && s_d[other].ia == 0xFFFF, "15.15.255 is no address: two may have it");
        vdev_prog(other, false);
        s_d[a].ia = s_d[b].ia = 0xFF20;   // records of earlier firmware
        for (int i = 0; i < VDEV_MAX; i++) if (s_d[i].used) save(&s_d[i]);
        wipe_ram();
        n0 = g_note_n[GW_NOTE_DUP];
        server_start();
        ck(g_note_n[GW_NOTE_DUP] == n0 + 1 && g_note_ia == 0xFF20, "two records with one address at the start: told");
    }
    // a device that comes back at a new short address is linked there with the parameters of its download (a built-in
    // class got its defaults); one that left stays marked so when it is linked (at the start); whether its receiver
    // is on is there to ask (vdev_rx_on)
    fresh();
    {
        static const uint8_t P10[10] = {1, 1, 100, 99, 1, 1, 100, 0, 0, 60};
        static const uint8_t IE[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
        int z = vdev_add(0x1234, 1, -1, IE);
        vdev_set_ia(z, 0xFF05);
        load4(z, P10, 10);
        restart_of(0xFF05);
        int p0 = g_params_n, s0 = g_zset_n;
        vdev_seen(0x4321, IE);
        ck(s_d[z].k == 0 && s_d[z].zaddr == 0x4321 && g_zset_n == s0 + 1 && g_params_n == p0 + 1 &&
           strstr(g_last_params, "\"p\":\"0101646301016400003c\"") != NULL,
           "back at another short address: linked there, with the parameters of the download again");
        int h = vdev_add(0x2000, 1, 1, NULL);   // a class given by hand, nothing downloaded
        s0 = g_zset_n;
        p0 = g_params_n;
        uint8_t ieh[8] = {9, 8, 7, 6, 5, 4, 3, 2};
        vdev_seen(0x2000, ieh);   // learns its IEEE address
        vdev_seen(0x2001, ieh);   // and moves
        ck(s_d[h].zaddr == 0x2001 && g_zset_n == s0 + 1 && g_params_n == p0 && s_d[h].k == 1,
           "a class given by hand follows its device with its own defaults, as before");
        ck(vdev_rx_on(z) == -1, "vdev_rx_on: not known before an announce");
        vdev_set_capability(IE, 0x8e);
        ck(vdev_rx_on(z) == 1, "vdev_rx_on: receiver on");
        vdev_set_capability(IE, 0x80);
        ck(vdev_rx_on(z) == 0, "vdev_rx_on: a sleepy end device");
        ck(vdev_rx_on(-1) == -1 && vdev_rx_on(VDEV_MAX - 1) == -1 && vdev_rx_on(VDEV_GW) == -1,
           "vdev_rx_on: no such device, or the gateway: not known");
        vdev_gone(z);   // its Zigbee device leaves the network
        reboot_clean();
        ck(vdev_rx_on(z) == 0, "vdev_rx_on after a restart: from the record");
        for (int i = 0; i < VDEV_MAX; i++) if (s_d[i].used) save(&s_d[i]);
        wipe_ram();
        g_gone_n = 0;
        server_start();
        ck(s_d[z].gone && s_d[z].k == 0 && g_gone_n == 1 && g_gone_inst == 100 + z && g_gone_val == 1,
           "a device whose Zigbee device had left: linked at the start and told the link it is gone");
        g_gone_n = 0;
        restart_of(0xFF05);
        ck(g_gone_n == 1 && g_gone_val == 1, "... also when it is linked again at a restart");
    }
    // a GroupValue_Response reaches an object with the update flag as a write does, and only such an object; more
    // group addresses than the link acknowledges are told on the gateway's message object
    fresh();
    {
        int a = vdev_add(0x1234, 1, 0, NULL);
        vdev_set_ia(a, 0xFF01);
        // KO 1 on 31/6/1 with the write flag, KO 2 on 31/6/2 with write and update flag
        static const uint8_t AT[6] = {0, 2, 0xF6, 0x01, 0xF6, 0x02}, AS[10] = {0, 2, 0, 1, 0, 1, 0, 2, 0, 2},
                             GO[6] = {0, 2, 0x17, 0x00, 0x97, 0x00};
        for (int o = 3; o >= 1; o--) prop_write5(a, o, UNLOAD);
        prop_write5(a, 3, START); alloc_obj(a, 3, sizeof GO, 1, 0); mem_write(a, 0x4800, GO, sizeof GO);
        prop_write5(a, 1, START); alloc_obj(a, 1, sizeof AT, 1, 0); mem_write(a, 0x4000, AT, sizeof AT);
        prop_write5(a, 2, START); alloc_obj(a, 2, sizeof AS, 1, 0); mem_write(a, 0x4400, AS, sizeof AS);
        for (int o = 3; o >= 1; o--) prop_write5(a, o, DONE);
        int n0 = g_kw_n;
        uint8_t rsp[2] = {0x00, 0x41};
        feed(0x1105, 0xF601, true, false, rsp, 2);
        ck(g_kw_n == n0, "a response to an object without the update flag: not taken");
        feed(0x1105, 0xF602, true, false, rsp, 2);
        ck(g_kw_n == n0 + 1 && g_kw_ko == 2, "a response to an object with the update flag: taken as a write");
        unsigned long g0 = g_note_n[GW_NOTE_GROUPS];
        for (int i = 0; i < 2; i++) {
            int d = vdev_add(0xFFFF, 1, 0, NULL);
            vdev_set_ia(d, (uint16_t)(0xFF02 + i));
            uint8_t tab[2 + 2 * 200];
            tab[0] = 0; tab[1] = 200;
            for (int k = 0; k < 200; k++) {
                uint16_t g = (uint16_t)(0x0800 + 256 * i + k);
                tab[2 + 2 * k] = (uint8_t)(g >> 8);
                tab[3 + 2 * k] = (uint8_t)g;
            }
            prop_write5(d, 1, START); alloc_obj(d, 1, sizeof tab, 1, 0);
            mem_write(d, 0x4000, tab, sizeof tab);
            prop_write5(d, 1, DONE);
        }
        ck(g_ack_nga == KNX_ACK_GA_MAX && g_note_n[GW_NOTE_GROUPS] == g0 + 1 && g_note_ia == 0xFF03,
           "402 group addresses, 256 acknowledged: told once, with the device whose addresses did not fit");
        vdev_prog(a, true);
        vdev_prog(a, false);
        ck(g_note_n[GW_NOTE_GROUPS] == g0 + 1, "published again without more: not told again");
    }
    // the console: a number that is no number is refused, not taken as 0; vdev and dump print nothing while the
    // device server's lock is held
    fresh();
    {
        int a = vdev_add(0x1234, 1, 0, NULL), b = vdev_add(0x2345, 1, 0, NULL);
        vdev_set_ia(a, 0xFF01);
        vdev_set_ia(b, 0xFF02);
        load_light(a);
        char *del[] = {"vdev", "del", "x", "yes"}, *ia[] = {"ia", "1x", "15.15.9"}, *re[] = {"reint", "x"},
             *ap[] = {"apdu", "55x"}, *pg[] = {"prog", "x", "on"}, *dm[] = {"dump", "0", "4x"};
        unsigned long l0 = g_leave_n, r0 = g_reint_n;
        ck(c_vdev(4, del) == 1 && s_d[0].used && g_leave_n == l0, "vdev del x yes: refused, device 0 stays (it was removed)");
        ck(c_ia(3, ia) == 1 && s_d[1].ia == 0xFF02, "ia 1x 15.15.9: refused");
        ck(c_reint(2, re) == 1 && g_reint_n == r0, "reint x: refused (it re-interviewed device 0)");
        ck(c_apdu(2, ap) == 1 && vdev_max_apdu(0) == 15, "apdu 55x: refused (atoi made 55 of it)");
        ck(c_prog(3, pg) == 1 && !s_d[0].prog, "prog x on: refused (device 0 went into programming mode)");
        ck(c_dump(3, dm) == 1, "dump 0 4x: refused");
        char *del0[] = {"vdev", "del", "1", "yes"};
        ck(c_vdev(4, del0) == 0 && !s_d[1].used && g_leave_n == l0 + 1, "vdev del 1 yes: removed, its Zigbee device asked to leave");
        g_print_locked = 0;
        vdev_print();
        vdev_dump(0, 2);
        gw_print();
        ck(g_print_locked == 0, "vdev, dump and gw print nothing while the device server's lock is held");
    }
    // what the class's values do after a start, when the device server holds nothing yet: given as initial they are
    // only kept (answered on a read); given as a status without initial they go out. zlink.c sends the first
    // "reachable" after a start that way and so tells the bus that a device which had been off is back.
    fresh();
    {
        int a = vdev_add(0x1234, 1, 0, NULL);
        vdev_set_ia(a, 0xFF01);
        load_light(a);
        reboot_clean();
        uint8_t one = 1, rd[2] = {0, 0};
        g_last_tx_n = 0;
        vdev_ko_out(100 + a, 4, &one, 1, true, true); pump();
        ck(g_last_tx_n == 0 && s_d[a].ko[4].have, "after a start, initial 1: kept, not sent");
        feed(0x1105, 0xF604, true, false, rd, 2);
        ck(g_last_tx_n == 1 && g_last_tx[0].tpdu[1] == 0x41, "... and answered on a read");
        reboot_clean();
        g_last_tx_n = 0;
        vdev_ko_out(100 + a, 4, &one, 1, false, true); pump();
        ck(g_last_tx_n == 1 && sent(0, 0xF604, "0081"), "after a start, a status 1 without initial: on the bus");
        g_last_tx_n = 0;
        vdev_ko_out(100 + a, 4, &one, 1, false, true); pump();
        ck(g_last_tx_n == 0, "the same status again: not sent twice");
    }
    // a template that does not come up at the start: the device runs nothing, as at run time (it ran the built-in class
    // of its application number before); a device set up by hand and never loaded still runs the class it was given
    fresh();
    {
        static const uint8_t SEG[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
        int a = vdev_add(0x1234, 1, 0, NULL), b = vdev_add(0x2345, 1, 1, NULL);
        vdev_set_ia(a, 0xFF01);
        vdev_set_ia(b, 0xFF02);
        load4(a, SEG, sizeof SEG);
        g_tpl_mode = 2;   // refused when it is brought up
        restart_of(0xFF01);
        ck(s_d[a].fault && s_d[a].k == -1, "at run time: refused, the device runs no class");
        for (int i = 0; i < VDEV_MAX; i++) if (s_d[i].used) save(&s_d[i]);
        wipe_ram();
        server_start();
        ck(s_d[a].fault == KNX_ERR_CONFIG && s_d[a].k == -1, "after a start: the same, no built-in class instead");
        ck(s_d[b].k == 1 && !s_d[b].fault, "a device set up by hand, never loaded: runs the class it was given");
        g_tpl_mode = 0;
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
        bool in = false;
        for (int i = 0; i < 40; i++) in = in || s_d[e].lo[1].seg[0x10 + i] != 0;
        ck(!in && g_last_tx_n == 1 && sent(0, A, "02404010"),
           "memory write of 40 octets with PID 56 = 15: not written, verify answers count 0 (03_03_07 p. 113)");
        {   // number and data octets must agree (08_03_07 2.7.11)
            uint8_t m[10] = {0x02, 0x84, 0x40, 0x10, 1, 2, 3, 4, 5, 6};
            feed(A, IA, false, false, m, 10);
            ck(s_d[e].lo[1].seg[0x10] == 0 && sent(0, A, "02404010"), "number 4 with 6 data octets: not written, count 0");
            feed(A, IA, false, false, m, 7);
            ck(s_d[e].lo[1].seg[0x10] == 0 && sent(0, A, "02404010"), "number 4 with 3 data octets: not written, count 0");
            uint8_t m12[16] = {0x02, 0x8C, 0x40, 0x10, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9};
            feed(A, IA, false, false, m12, 16);
            ck(s_d[e].lo[1].seg[0x10] == 9 && s_d[e].lo[1].seg[0x1B] == 9 && g_last_tx_n == 1 && g_last_tx[0].len == 16,
               "number 12 with PID 56 = 15: written, echo of 16 octets");
        }
        uint8_t r[4] = {0x02, 40, 0x40, 0x10};   // A_Memory_Read, 40 octets
        feed(A, IA, false, false, r, 4);
        ck(g_last_tx_n == 1 && sent(0, A, "02404010"), "memory read of 40 octets with PID 56 = 15: count 0");
        ck(vdev_max_apdu(66) == 66 && vdev_max_apdu(67) == 66 && vdev_max_apdu(14) == 66 && vdev_max_apdu(0) == 66,
           "apdu 66 taken, 67 and 14 refused, 0 reads");
        pread(e, 0, 56, 1, 1);
        ck(g_last_tx_n == 1 && g_last_tx[0].tpdu[7] == 66, "PID 56 reads 66 then");
        feed(A, IA, false, false, t, 44);
        in = true;
        for (int i = 0; i < 40; i++) in = in && s_d[e].lo[1].seg[0x10 + i] == (uint8_t)(0xA0 + i);
        snprintf((char *)want, sizeof want, "0268%s", "4010");
        for (int i = 0; i < 40; i++) sprintf((char *)want + 8 + 2 * i, "%02x", 0xA0 + i);
        ck(in && g_last_tx_n == 1 && g_last_tx[0].len == 44 && sent(0, A, (char *)want),
           "memory write of 40 octets with PID 56 = 66 (extended frame): written, echo of 44 octets with the data");
        feed(A, IA, false, false, r, 4);
        ck(g_last_tx_n == 1 && g_last_tx[0].len == 44 && sent(0, A, (char *)want), "memory read of 40 octets with PID 56 = 66");
        {   // a table as an array (PID_TABLE): as many entries as the frame takes, PID 56 - 5 octets of data
            static const uint8_t AS4[18] = {0, 4, 0, 1, 0, 1, 0, 2, 0, 2, 0, 3, 0, 3, 0, 4, 0, 4};
            prop_write5(e, 2, START);
            alloc_obj(e, 2, sizeof AS4, 1, 0);
            mem_write(e, 0x4400, AS4, sizeof AS4);
            prop_write5(e, 2, DONE);
            pread(e, 2, 23, 3, 1);
            ck(sent(0, A, "03d60217300100010001000200020003" "0003"),
               "PID 56 = 66: three association entries (12 octets) in one answer");
            vdev_max_apdu(15);
            pread(e, 2, 23, 3, 1);
            ck(sent(0, A, "03d602170001"), "PID 56 = 15: the same three do not fit, count 0");
            vdev_max_apdu(66);
        }
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
    // the gateway's parameters: LongFrames (octet 4) gives PID 56 of every device from the restart that ends the
    // gateway's download; parameters the gateway cannot take end that download in Error
    fresh();
    {
        int e = vdev_add(0xFFFF, 1, 0, NULL);
        vdev_set_ia(e, IA);
        init_dev(&s_d[GW]);
        s_d[GW].ia = 0xFF30;
        publish();
        static const uint8_t L1[5] = {1, 1, 47, 180, 1}, L2[5] = {1, 1, 47, 180, 2}, L0[5] = {1, 1, 47, 180, 0},
                             V2[4] = {1, 1, 47, 180};
        vdev_max_apdu(15);
        gw_load(L1, 5);
        pread(e, 0, 56, 1, 1);
        ck(s_d[GW].lo[4].state == LS_LOADED && sent(0, A, "03d600381001000f"),
           "LongFrames 1 loaded: PID 56 stays 15 until the gateway restarts");
        restart_of(0xFF30);
        pread(e, 0, 56, 1, 1);
        ck(sent(0, A, "03d6003810010037"), "after the gateway's restart: PID 56 = 55 in every device");
        pread(GW, 0, 56, 1, 1);
        ck(sent(0, A, "03d6003810010037"), "... and in the gateway itself");
        gw_load(L2, 5);
        restart_of(0xFF30);
        pread(e, 0, 56, 1, 1);
        ck(sent(0, A, "03d6003810010042"), "LongFrames 2: PID 56 = 66");
        gw_load(L0, 5);
        restart_of(0xFF30);
        pread(e, 0, 56, 1, 1);
        ck(sent(0, A, "03d600381001000f"), "LongFrames 0: standard frames, PID 56 = 15");
        gw_load(L2, 5);
        restart_of(0xFF30);
        gw_load(V2, 4);
        restart_of(0xFF30);
        pread(e, 0, 56, 1, 1);
        ck(sent(0, A, "03d600381001000f"), "application version 2 (4 octets, no LongFrames): standard frames");
        vdev_max_apdu(66);
        pread(e, 0, 56, 1, 1);
        ck(sent(0, A, "03d6003810010042"), "the console overrides it ...");
        restart_of(0xFF30);
        pread(e, 0, 56, 1, 1);
        ck(sent(0, A, "03d600381001000f"), "... until the gateway's next restart");
        gw_load(L1, 5);
        restart_of(0xFF30);
        reboot_clean();
        apply(&s_d[GW]);   // as vdev_start() does with the gateway's record
        pread(e, 0, 56, 1, 1);
        ck(sent(0, A, "03d6003810010037"), "after a start: from the gateway's record, 55");
        static const uint8_t SWAP[5] = {1, 47, 1, 180, 1}, TOP[5] = {1, 1, 255, 180, 1}, BAD[5] = {1, 1, 47, 180, 3};
        int f0 = vdev_faulty();
        gw_load(SWAP, 5);
        ck(s_d[GW].lo[4].state == LS_ERROR && s_d[GW].lo[4].err == KNX_ERR_CONFIG && sent(0, A, "03d60405100103"),
           "first device number above the last: the gateway's download ends in Error 3");
        ck(vdev_faulty() == f0 + 1, "the gateway's own application in Error counts for its fault object");
        gw_load(TOP, 5);
        ck(s_d[GW].lo[4].state == LS_ERROR, "last device 255 in line 15.15: 15.15.255 is no device address, Error");
        gw_load(BAD, 5);
        ck(s_d[GW].lo[4].state == LS_ERROR && s_d[GW].lo[4].err == KNX_ERR_CONFIG, "LongFrames 3: Error 3");
        gw_load(L2, 5);
        ck(s_d[GW].lo[4].state == LS_LOADED && vdev_faulty() == f0, "a good download again: Loaded, no fault");
        vdev_max_apdu(15);
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
