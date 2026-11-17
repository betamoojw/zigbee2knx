// Host test of the endpoint rule (main/ep_pick.h). The descriptors are made up to cover the rule; none of them was
// captured from a real device. Ends with "endpoint checks: N passed, M failed".
#include <stdio.h>
#include <string.h>
#include "ep_pick.h"

static int g_pass, g_fail;
static void ck(int ok, const char *what)
{
    if (ok) {
        g_pass++;
    } else {
        g_fail++;
        printf("FAIL: %s\n", what);
    }
}

static ep_desc_t mk(uint8_t ep, uint16_t profile, const uint16_t *in, int nin, const uint16_t *out, int nout)
{
    ep_desc_t d;
    memset(&d, 0, sizeof d);
    d.ep = ep;
    d.profile = profile;
    d.nin = (uint8_t)nin;
    d.nout = (uint8_t)nout;
    if (nin) {
        memcpy(d.cl, in, (size_t)nin * 2);
    }
    if (nout) {
        memcpy(d.cl + nin, out, (size_t)nout * 2);
    }
    return d;
}

int main(void)
{
    static const uint16_t LIGHT[] = {0x0000, 0x0003, 0x0004, 0x0005, 0x0006, 0x0008, 0x0300, 0x1000};
    static const uint16_t DIM[] = {0x0000, 0x0003, 0x0004, 0x0005, 0x0006, 0x0008, 0x1000};
    static const uint16_t PLUG[] = {0x0000, 0x0003, 0x0004, 0x0005, 0x0006};
    static const uint16_t METER[] = {0x0000, 0x0702, 0x0b04};
    static const uint16_t BASIC[] = {0x0000, 0x0003};
    static const uint16_t REMOTE_IN[] = {0x0000, 0x0001, 0x0003, 0x1000};
    static const uint16_t REMOTE_OUT[] = {0x0003, 0x0004, 0x0006, 0x0008, 0x0019};
    static const uint16_t GP[] = {0x0021};
    ep_desc_t d[4];

    d[0] = mk(1, 0x0104, DIM, 7, NULL, 0);
    d[1] = mk(242, 0xA1E0, NULL, 0, GP, 1);
    ck(ep_pick(d, 2) == 1, "dimmable light on 1, Green Power on 242: 1");

    d[0] = mk(11, 0x0104, LIGHT, 8, NULL, 0);
    d[1] = mk(242, 0xA1E0, NULL, 0, GP, 1);
    ck(ep_pick(d, 2) == 11, "light on 11 (as Hue lights have it): 11");

    d[0] = mk(11, 0xC05E, LIGHT, 8, NULL, 0);
    ck(ep_pick(d, 1) == 11, "Light Link profile counts");

    d[0] = mk(1, 0x0104, REMOTE_IN, 4, REMOTE_OUT, 5);
    ck(ep_pick(d, 1) == 1, "remote: buttons from 1");

    d[0] = mk(1, 0x0104, BASIC, 2, NULL, 0);
    d[1] = mk(2, 0x0104, PLUG, 5, NULL, 0);
    ck(ep_pick(d, 2) == 2, "on/off on 2, nothing on 1: 2");

    d[0] = mk(1, 0x0104, PLUG, 5, NULL, 0);
    d[1] = mk(2, 0x0104, PLUG, 5, NULL, 0);
    ck(ep_pick(d, 2) == 1, "two switched outputs: the lower one");

    d[0] = mk(2, 0x0104, PLUG, 5, NULL, 0);
    d[1] = mk(1, 0x0104, METER, 3, NULL, 0);
    ck(ep_pick(d, 2) == 2, "switch on 2, metering on 1: the switch");

    d[0] = mk(3, 0x0104, BASIC, 2, NULL, 0);
    d[1] = mk(5, 0x0104, BASIC, 2, NULL, 0);
    ck(ep_pick(d, 2) == 3, "nothing known: the lowest that counts");

    d[0] = mk(242, 0xA1E0, NULL, 0, GP, 1);
    ck(ep_pick(d, 1) == 0, "only Green Power: none");
    ck(ep_pick(d, 0) == 0, "no endpoint: none");

    d[0] = mk(1, 0xC105, PLUG, 5, NULL, 0);   // a manufacturer profile
    d[1] = mk(9, 0x0104, BASIC, 2, NULL, 0);
    ck(ep_pick(d, 2) == 9, "an unknown profile does not count, even with On/Off");

    d[0] = mk(1, 0x0104, LIGHT, 8, NULL, 0);
    d[1] = mk(2, 0x0104, DIM, 7, NULL, 0);
    ck(ep_pick(d, 2) == 1, "colour light beats a dimmable one: the higher score");

    d[0] = mk(4, 0x0104, PLUG, 5, NULL, 0);
    d[1] = mk(3, 0x0104, REMOTE_IN, 4, REMOTE_OUT, 5);
    ck(ep_pick(d, 2) == 4, "an actuator endpoint beats a remote endpoint");

    printf("endpoint checks: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail != 0;
}
