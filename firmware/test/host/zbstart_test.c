// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dirk Tostmann
// Host test of main/zbstart.h: whether the gateway forms a new Zigbee network when the stack starts without the one it
// had. Ends with "zbstart checks: N passed, M failed" and exits non-zero on a failure.
#include <stdio.h>
#include "zbstart.h"

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

int main(void)
{
    ck(zbstart_decide(true, false, 0, false) == ZBSTART_RESTORED, "network restored: nothing to decide");
    ck(zbstart_decide(true, true, 3, true) == ZBSTART_RESTORED, "restored wins over a request for a new one");
    ck(zbstart_decide(false, true, 0, false) == ZBSTART_FORM, "a first start, no device paired: form");
    ck(zbstart_decide(false, true, 3, false) == ZBSTART_REFUSE,
       "three devices paired and the network gone (data lost or unreadable after an update): do not form");
    ck(zbstart_decide(false, true, 1, false) == ZBSTART_REFUSE, "one device waiting for an address is paired too");
    ck(zbstart_decide(false, false, 0, false) == ZBSTART_WAIT, "records not read yet: wait");
    ck(zbstart_decide(false, false, 0, true) == ZBSTART_FORM, "zbreset or zbform asked: form without waiting");
    ck(zbstart_decide(false, true, 3, true) == ZBSTART_FORM, "zbform with devices paired: form");
    printf("zbstart checks: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
