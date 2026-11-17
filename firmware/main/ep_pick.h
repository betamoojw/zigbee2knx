// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dirk Tostmann
// Which endpoint of a Zigbee device a class talks to (the full interview). Earlier firmware took every device on
// endpoint 1; a Philips Hue light, for one, has its light on endpoint 11. Kept apart from the stack so
// that the host tests run it (test/host/ep_pick_test.c).
//
// The interview asks the device for its active endpoints (ZDO Active_EP_req) and for the simple descriptor of each
// (Simple_Desc_req): profile and the clusters it serves (in) and uses (out). The rule:
//   - endpoint 242 (Green Power) and profiles other than Home Automation (0x0104) and Light Link (0xC05E) do not count
//   - an endpoint that serves On/Off (0x0006) is what the actuator classes switch: 4 points, Level Control (0x0008)
//     and Color Control (0x0300) served 1 point each
//   - an endpoint that uses On/Off or Level Control as a client is where a remote's buttons come from: 2 points
//   - the most points win, the lower endpoint on a tie; without points the lowest endpoint that counts
// 0: no endpoint counts (keep what the device has).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define EP_CL_MAX   24   // clusters kept per endpoint, in and out together
#define EP_DESC_MAX 8    // endpoints kept per device

typedef struct {
    uint8_t ep;
    uint16_t profile;
    uint8_t nin, nout;          // in (server) clusters first, then out (client), as many as fitted into cl[]
    uint16_t cl[EP_CL_MAX];
} ep_desc_t;

static inline bool ep_has(const ep_desc_t *d, bool in, uint16_t cluster)
{
    int a = in ? 0 : d->nin, b = in ? d->nin : d->nin + d->nout;
    for (int i = a; i < b && i < EP_CL_MAX; i++) {
        if (d->cl[i] == cluster) {
            return true;
        }
    }
    return false;
}

static inline bool ep_counts(const ep_desc_t *d)
{
    return d->ep >= 1 && d->ep <= 240 && (d->profile == 0x0104 || d->profile == 0xC05E);
}

static inline int ep_score(const ep_desc_t *d)
{
    if (!ep_counts(d)) {
        return -1;
    }
    int s = 0;
    if (ep_has(d, true, 0x0006)) {
        s += 4;
    }
    s += ep_has(d, true, 0x0008) + ep_has(d, true, 0x0300);
    if (!s && (ep_has(d, false, 0x0006) || ep_has(d, false, 0x0008))) {
        s = 2;
    }
    return s;
}

static inline uint8_t ep_pick(const ep_desc_t *d, int n)
{
    int best = -1;
    uint8_t ep = 0;
    for (int i = 0; i < n; i++) {
        int s = ep_score(&d[i]);
        if (s > best || (s == best && s >= 0 && d[i].ep < ep)) {
            best = s;
            ep = d[i].ep;
        }
    }
    return best >= 0 ? ep : 0;
}
