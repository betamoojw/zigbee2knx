#define main cases_main
#include "vdev_cases.c"
#undef main
int main(void)
{
    vdev_start();
    fresh();
    int d = vdev_add(0xFFFF, 1, 0, NULL);
    vdev_set_ia(d, 0xFF01);
    tick_for(3000000);
    vdev_set_ia(d, 0xFF02);            // dirty
    unsigned long e0 = g_log['E'], s0 = s_saves;
    g_alloc_fail_pct = 100;            // no memory for the record buffer
    tick_for(10000000);
    g_alloc_fail_pct = 0;
    say("H: 10 s without memory for save(): %lu error lines, %lu saves, dirty %d", g_log['E'] - e0, s_saves - s0, s_d[d].dirty);
    // publish(): more group addresses than the list takes
    fresh();
    for (int i = 0; i < 2; i++) {
        d = vdev_add(0xFFFF, 1, 0, NULL);
        vdev_set_ia(d, (uint16_t)(0xFF01 + d));
        uint8_t tab[2 + 2 * 100];
        tab[0] = 0; tab[1] = 100;
        for (int k = 0; k < 100; k++) { uint16_t g = (uint16_t)(0x0800 + 256 * i + k); tab[2 + 2 * k] = (uint8_t)(g >> 8); tab[3 + 2 * k] = (uint8_t)g; }
        prop_write5(d, 1, START); alloc_obj(d, 1, sizeof tab, 1, 0);
        mem_write(d, 0x4000, tab, sizeof tab);
        prop_write5(d, 1, DONE);
    }
    say("H: two devices with 100 different group addresses each: %d on the acknowledge list (limit %d)", g_ack_nga, KNX_ACK_GA_MAX);
    return 0;
}
