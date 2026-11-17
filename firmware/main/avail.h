// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dirk Tostmann
// Is a linked Zigbee device there? The decision for one device, kept apart from the Zigbee stack so that the host
// tests run it (test/host/avail_test.c). zlink.c feeds it what the stack sees and acts on what it says (a device that
// does not answer is told on the gateway's message object, "15.15.2 OFF").
//
// What counts:
//   heard   any ZCL frame from the device (report, answer, default response, a command of a remote) or its device
//           announce. Counted by the Zigbee task in heard_n.
//   failed  the APS confirm of one of our unicasts to the device with a status other than 0 (0xA7: no APS ack).
//           Counted by the Zigbee task in fail_n.
// The rules (the times follow the reporting this firmware configures: On/Off at least every 600 s, battery every 6 h):
//   receiver on when idle (lamps, plugs: mains powered routers)
//       off after AV_FAILS failed sends without anything heard in between, or after AV_SILENT_US with nothing heard.
//       After AV_PING_US of silence the device is asked (a read of the Basic cluster) every AV_PING_EVERY_US, so that
//       a quiet one answers before it counts as gone, and one that is off is noticed when it is back.
//   sleepy end device (remotes, sensors on battery: the MAC capability of its device announce says receiver off)
//       a failed send says nothing, the device sleeps. Off after AV_SLEEPY_US with nothing heard (25 h, the default
//       Zigbee2MQTT uses for battery devices). Never asked.
//   on again with the first frame heard after it went off.
// Before the first frame after linking the state is unknown; the times run from the moment of linking.
#pragma once
#include <stdbool.h>
#include <stdint.h>

enum { AV_UNKNOWN = 0, AV_ON, AV_OFF };

#define AV_FAILS         2u
#define AV_PING_US       (11LL * 60 * 1000000)     // silent this long: ask
#define AV_PING_EVERY_US (5LL * 60 * 1000000)      // ... and again after this, as long as it stays silent
#define AV_SILENT_US     (30LL * 60 * 1000000)     // silent this long: off, whatever the sends said
#define AV_SLEEPY_US     (25LL * 3600 * 1000000)   // a sleepy end device: off after this long without a frame

typedef struct {
    uint8_t state;        // AV_*
    bool sleepy;          // the device's receiver is off when idle
    uint32_t heard_seen;  // heard_n at the last decision
    uint32_t fail_base;   // fail_n when something was heard last: failures before it do not count
    int64_t last_us;      // when something was heard last, or when the link began
    int64_t ping_us;      // when the last question went out
} av_t;

// A new link: nothing heard yet, the clock starts now. heard_n and fail_n are the counters as they stand.
static inline void av_init(av_t *a, int64_t now, bool sleepy, uint32_t heard_n, uint32_t fail_n)
{
    a->state = AV_UNKNOWN;
    a->sleepy = sleepy;
    a->heard_seen = heard_n;
    a->fail_base = fail_n;
    a->last_us = now;
    a->ping_us = now;   // the first question not before AV_PING_US: linking reads the device anyway
}

// One decision with the counters as they stand now. Returns the state; *ping: ask the device now.
static inline uint8_t av_step(av_t *a, uint32_t heard_n, uint32_t fail_n, int64_t now, bool *ping)
{
    *ping = false;
    if (heard_n != a->heard_seen) {   // something came from the device since the last decision
        a->heard_seen = heard_n;
        a->fail_base = fail_n;
        a->last_us = now;
        a->state = AV_ON;
        return a->state;
    }
    int64_t quiet = now - a->last_us;
    if (a->sleepy) {
        if (quiet > AV_SLEEPY_US) {
            a->state = AV_OFF;
        }
        return a->state;
    }
    if (fail_n - a->fail_base >= AV_FAILS || quiet > AV_SILENT_US) {
        a->state = AV_OFF;
    }
    if (quiet > AV_PING_US && now - a->ping_us >= AV_PING_EVERY_US) {   // also while off: it may be back
        a->ping_us = now;
        *ping = true;
    }
    return a->state;
}
