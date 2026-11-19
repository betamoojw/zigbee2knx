// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dirk Tostmann
// Whether a template may load into a VM of its own (bvm.c tpl_load_vm()): the heap a load takes in all and in one
// piece, against the free heap and its largest free block just before the load. Kept apart so that the host tests run
// it (test/host/tplmem_test.c).
//
// In all: measured on 2026-11-13 with the load of then (below), the heap falls by 39.2 KB loading 3 KB of source and
// by 80.1 KB loading 17.9 KB, about 31 KB + 2.75 x payload; on top of that the 64 KB left for Zigbee and the bus.
// Measured on 2026-11-19 on the C6 with the load of today (a development build of 0.6.1, the peak tpl_load() logs):
// 29680 B for the switch (3229 B payload), 58304 B for the dimmable light (11282 B), 91684 B and 92224 B for the white
// spectrum light v5 and v6 (19860 B, 20124 B). Between the two light classes the peak grows by about 3.8 B per octet
// of payload. The rule of 0.6.1, 32 KB + 3 x payload, was then 916 B above the peak of C002 v6, and from about 21 KB of
// payload on a load would have taken some of the 64 KB reserve. Since 2026-11-19 (after 0.6.1) the load counts the
// larger of 32 KB + 3 x payload and 16 KB + 4 x payload: the same up to 16 KB of payload, 4 B per octet above it
// (C002 v6: 96880 B against the 92224 B measured). Above 20 KB of payload that is extrapolated from three points.
//
// In one piece, since 2026-11-19: the behaviour compiles straight from the payload's own buffer, which exists before
// the check (kgtjson.h, bvm.c m_behaviour()). New after the check are
//   - the payload without its behaviour (rlen octets: the manifest with its model list) in C, and its copy as a Berry
//     string (bvm.c m_payload()), alive at the same time; json.load() then makes only short strings of it;
//   - what the compiler grows while it reads the behaviour, the largest being the lexer's table of strings. Measured on
//     2026-11-19 on a 64-bit PC with the firmware's Berry and the four templates given out (test/host/tplmem_test.c):
//     0.73 to 0.83 x the behaviour for the three larger ones, 3072 B for the switch's 2144 B. The table grows in steps,
//     so right after a step it can be about twice as much: the rule takes 2 x the behaviour as escaped in the payload
//     (no shorter than the source), and no less than 8 KB, which a small template can take after a step.
// The largest free block has to take the larger of the two, with the headers of the blocks. Assumed, and not measured
// on the C6: the VM, the prelude and the boot code (about 20 KB in pieces of at most a few KB, before the payload) do
// not leave less of the largest block than the margin. The gateway logs, for every load, the largest block Berry took
// (be_os_mem_get(BE_OS_MEM_LARGEST)), so that the rule can be held against what the C6 does.
//
// History: until 2026-11-19 the whole payload went into a Berry string and json.load() made the behaviour into a
// buffer and a string of its own; the rule asked for one block of 2 x payload + 4096 B, and after an update in the ETS
// a white spectrum template of 20124 B was refused with a largest block of 44032 B and 174616 B free.
#ifndef TPLMEM_H
#define TPLMEM_H

#include <stddef.h>

#define TPLMEM_SLACK 512          // per block: heap and Berry string headers and rounding (a few dozen octets)
#define TPLMEM_COMPILE_MIN 8192   // the compiler's largest block for a small template, a step included

typedef enum { TPLMEM_OK, TPLMEM_TOTAL, TPLMEM_BLOCK } tplmem_t;

static inline size_t tplmem_load(size_t plen)    // heap the load itself takes at its peak (the lines above)
{
    size_t small = 32 * 1024 + 3 * plen, large = 16 * 1024 + 4 * plen;
    return small > large ? small : large;
}

static inline size_t tplmem_want(size_t plen)    // free heap a load takes, the reserve for Zigbee and the bus included
{
    return tplmem_load(plen) + 64 * 1024;
}

// largest free block a load takes; rlen: the payload without its behaviour, blen: the behaviour as the payload
// carries it (escapes in)
static inline size_t tplmem_block(size_t rlen, size_t blen)
{
    size_t rest = 2 * (rlen + TPLMEM_SLACK);
    size_t compile = (2 * blen > TPLMEM_COMPILE_MIN ? 2 * blen : TPLMEM_COMPILE_MIN) + TPLMEM_SLACK;
    return rest > compile ? rest : compile;
}

// free_extra: the heap of the VM the device gives up before the new one loads. The check before a download completes
// counts it as free; which blocks it leaves is unknown, so there a largest block that is too small passes only if that
// VM could make up for it. The load itself comes after the VM is gone and passes 0.
static inline tplmem_t tplmem_check(size_t free_now, size_t largest, size_t plen, size_t rlen, size_t blen,
                                    size_t free_extra)
{
    if (free_now + free_extra < tplmem_want(plen)) {
        return TPLMEM_TOTAL;
    }
    size_t need = tplmem_block(rlen, blen);
    if (largest < need && free_extra < need) {
        return TPLMEM_BLOCK;
    }
    return TPLMEM_OK;
}

#endif
