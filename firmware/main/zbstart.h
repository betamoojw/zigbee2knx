// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dirk Tostmann
// What the gateway does when the Zigbee stack starts without the network it had (zb.c, the BDB start signal): a
// gateway whose device server still holds paired Zigbee devices does not form a new network by itself. A new network
// takes every device away, each one has to be reset and join anew; that is left to the console ("zbform yes") or to
// "zbreset yes". Before the device server has read its records the answer is not known yet, and the decision waits.
// Kept apart so that the host tests run it (test/host/zbstart_test.c).
#ifndef ZBSTART_H
#define ZBSTART_H

#include <stdbool.h>

typedef enum {
    ZBSTART_RESTORED,   // the stack restored its network
    ZBSTART_WAIT,       // the device server has not read its records yet: decide again a little later
    ZBSTART_FORM,       // no Zigbee device is paired, or a new network was asked for: form one
    ZBSTART_REFUSE,     // Zigbee devices are paired and their network is gone: do not form, report it
} zbstart_t;

// restored: the start signal said success and the stack is not factory new; loaded: the device server has read its
// records; paired: the Zigbee devices it holds, with and without an individual address; asked: "zbform yes" or
// "zbreset yes" asked for a new network
static inline zbstart_t zbstart_decide(bool restored, bool loaded, int paired, bool asked)
{
    if (restored) {
        return ZBSTART_RESTORED;
    }
    if (asked) {
        return ZBSTART_FORM;
    }
    if (!loaded) {
        return ZBSTART_WAIT;
    }
    return paired > 0 ? ZBSTART_REFUSE : ZBSTART_FORM;
}

#endif
