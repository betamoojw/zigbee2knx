// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dirk Tostmann
// Host test of the availability rules (main/avail.h): scenarios on a virtual clock, every decision as the link task
// makes it (a tick every 10 s, plus one right after something was heard or a send failed). Prints each case and ends
// with "availability checks: N passed, M failed".
#include <stdio.h>
#include "avail.h"

#define S  1000000LL
#define MIN (60 * S)
#define H  (60 * MIN)

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

typedef struct {
    av_t a;
    uint32_t heard, fail;   // the counters the Zigbee task keeps
    int64_t now;
    int pings, offs, ons;
    int64_t off_at, on_at;
} sim_t;

static uint8_t step(sim_t *d)
{
    bool ping;
    uint8_t from = d->a.state, to = av_step(&d->a, d->heard, d->fail, d->now, &ping);
    d->pings += ping;
    if (to != from && to == AV_OFF) {
        d->offs++;
        d->off_at = d->now;
    }
    if (to != from && from == AV_OFF) {
        d->ons++;
        d->on_at = d->now;
    }
    return to;
}

// let time pass in ticks of 10 s; heard_every > 0: the device sends something that often (reports)
static void run(sim_t *d, int64_t dur, int64_t heard_every, bool ping_answered, bool ping_fails)
{
    int64_t end = d->now + dur, next_heard = heard_every > 0 ? d->now + heard_every : INT64_MAX;
    while (d->now < end) {
        d->now += 10 * S;
        if (d->now >= next_heard) {
            d->heard++;
            next_heard += heard_every;
        }
        int before = d->pings;
        step(d);
        if (d->pings != before) {   // the question went out: its answer, or its failed confirm a few seconds later
            d->now += 2 * S;
            if (ping_answered) {
                d->heard++;
            } else if (ping_fails) {
                d->fail++;
            }
            step(d);
        }
    }
}

static void init(sim_t *d, bool sleepy)
{
    *d = (sim_t){.now = 1000 * S};
    av_init(&d->a, d->now, sleepy, 0, 0);
}

int main(void)
{
    sim_t d;

    printf("A lamp that answers its first reads and reports every 10 min stays on for a day, never asked\n");
    init(&d, false);
    d.now += 3 * S;
    d.heard += 4;
    ck(step(&d) == AV_ON, "first answers: on");
    run(&d, 24 * H, 10 * MIN, true, false);
    ck(d.a.state == AV_ON && d.offs == 0 && d.pings == 0, "a day with reports: on, no question");
    printf("  pings %d, went off %d times\n", d.pings, d.offs);

    printf("B a lamp without reporting: asked after 11 min of silence, answers, stays on\n");
    init(&d, false);
    d.heard++;
    step(&d);
    run(&d, 3 * H, 0, true, false);
    ck(d.a.state == AV_ON && d.offs == 0, "answers every question: on");
    ck(d.pings >= 14 && d.pings <= 17, "about one question per 11 to 12 min");
    printf("  3 h: %d questions\n", d.pings);

    printf("C a lamp switched off at the wall: questions fail, off after two failures\n");
    init(&d, false);
    d.heard++;
    step(&d);
    run(&d, 10 * MIN, 10 * MIN, true, false);   // reports until it goes dark
    int64_t dark = d.now;
    run(&d, 40 * MIN, 0, false, true);
    ck(d.a.state == AV_OFF && d.offs == 1, "off");
    ck(d.off_at - dark <= 17 * MIN + 30 * S, "within the first two questions (11 + 5 min)");
    printf("  off %lld s after it went dark\n", (long long)((d.off_at - dark) / S));
    d.heard++;   // power back: its device announce
    ck(step(&d) == AV_ON && d.ons == 1, "the announce brings it back at once");

    printf("D commands to a lamp that is off: two failed sends are enough\n");
    init(&d, false);
    d.heard++;
    step(&d);
    d.now += 5 * S;
    d.fail++;
    ck(step(&d) == AV_ON, "one failed send: still on");
    d.now += 2 * S;
    d.fail++;
    ck(step(&d) == AV_OFF, "the second: off");

    printf("E a failure, then an answer, then a failure: never two in a row, stays on\n");
    init(&d, false);
    d.heard++;
    step(&d);
    d.now += 5 * S;
    d.fail++;
    step(&d);
    d.now += 5 * S;
    d.heard++;
    step(&d);
    d.now += 5 * S;
    d.fail++;
    ck(step(&d) == AV_ON, "on");

    printf("F silence without any confirm (the stack says nothing): off after 30 min\n");
    init(&d, false);
    d.heard++;
    step(&d);
    int64_t t0 = d.now;
    run(&d, 40 * MIN, 0, false, false);
    ck(d.a.state == AV_OFF && d.off_at - t0 > 30 * MIN && d.off_at - t0 <= 30 * MIN + 20 * S, "off at 30 min");

    printf("G a lamp that is off when the gateway starts: its first reads fail\n");
    init(&d, false);
    d.now += 2 * S;
    d.fail++;
    ck(step(&d) == AV_UNKNOWN, "one failed read: still unknown");
    d.now += 1 * S;
    d.fail++;
    ck(step(&d) == AV_OFF, "the second: off, a few seconds after the start");

    printf("H a remote (sleepy end device): failed sends say nothing, off after 25 h of silence\n");
    init(&d, true);
    d.now += 2 * S;
    d.fail += 4;   // the first reads after the start: it sleeps
    ck(step(&d) == AV_UNKNOWN, "four failed reads: unknown, not off");
    run(&d, 6 * H, 0, false, false);
    ck(d.pings == 0, "never asked");
    d.heard++;   // battery report
    ck(step(&d) == AV_ON, "its battery report: on");
    run(&d, 24 * H, 0, false, false);
    ck(d.a.state == AV_ON, "24 h quiet: still on");
    run(&d, 2 * H, 0, false, false);
    ck(d.a.state == AV_OFF && d.offs == 1, "after 25 h: off");
    d.heard++;
    ck(step(&d) == AV_ON, "a button press: on");
    run(&d, 48 * H, 6 * H, false, false);   // battery report every 6 h
    ck(d.a.state == AV_ON && d.offs == 1, "reports every 6 h: stays on");

    printf("I counters that wrap\n");
    init(&d, false);
    d.heard = 0xFFFFFFFFu;
    d.fail = 0xFFFFFFFEu;
    av_init(&d.a, d.now, false, d.heard, d.fail);
    d.now += S;
    d.heard++;   // wraps to 0
    ck(step(&d) == AV_ON, "heard across the wrap");
    d.fail++;
    d.fail++;    // wraps to 0
    d.now += S;
    ck(step(&d) == AV_OFF, "two failures across the wrap");

    printf("J an off lamp keeps being asked every 5 min, and its answer brings it back\n");
    init(&d, false);
    d.heard++;
    step(&d);
    run(&d, 60 * MIN, 0, false, true);
    int pings_off = d.pings;
    ck(d.a.state == AV_OFF && pings_off >= 10, "off, questions go on");
    run(&d, 6 * MIN, 0, true, false);
    ck(d.a.state == AV_ON && d.ons == 1, "the answer to the next question: on");

    printf("availability checks: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail != 0;
}
