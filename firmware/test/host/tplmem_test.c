// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dirk Tostmann
// Host test of the memory rule for loading a template (main/tplmem.h): the case the gateway refused after an update in
// the ETS (2026-11-19), the four templates given out with what their load took on a 64-bit PC, the edges. Ends with
// "tplmem checks: N passed, M failed" and exits non-zero on a failure.
#include <stdio.h>
#include "tplmem.h"

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

// The templates given out on 2026-11-19, split as bvm.c does it (kgtjson.h): payload, payload without the behaviour,
// behaviour as escaped in the payload; and the largest single allocation of their load (JSON of the rest and the
// compiler), measured with the firmware's Berry on a 64-bit PC (a counting allocator around the load sequence of
// bvm.c, review G).
static const struct {
    const char *name;
    size_t plen, rlen, blen, took;
} T[] = {
    {"C002 v6 white spectrum", 20124, 4591, 15533, 12288},
    {"C001 v5 dimmable", 11282, 2650, 8632, 6144},
    {"C004 v3 remote", 6367, 1217, 5150, 4096},
    {"C003 v4 switch", 3229, 1010, 2219, 3072},
};

int main(void)
{
    // 2026-11-19, 15.15.1 after the update to C002 v6: the load itself (the device had given up its VM) saw 174616 B
    // free and a largest block of 44032 B; the rule then asked one block of 2 x 20124 + 4096 = 44344 B
    ck(tplmem_want(20124) == 158676, "in all: 32 KB + 3 x payload + 64 KB, as logged (158676 B)");
    ck(2 * 20124 + 4096 > 44032, "the rule before refuses the logged case (44344 B > 44032 B)");
    ck(tplmem_block(4591, 15533) == 31578, "C002 v6 in one piece now: 2 x 15533 + 512 = 31578 B");
    ck(tplmem_check(174616, 44032, 20124, 4591, 15533, 0) == TPLMEM_OK, "the logged case loads now");
    for (unsigned i = 0; i < sizeof T / sizeof T[0]; i++) {
        size_t need = tplmem_block(T[i].rlen, T[i].blen);
        char what[160];
        snprintf(what, sizeof what, "%s: need %zu B holds what its load took (%zu B) and the rest beside it (%zu B)",
                 T[i].name, need, T[i].took, T[i].rlen);
        ck(need >= T[i].took + T[i].rlen, what);
        snprintf(what, sizeof what, "%s: need %zu B holds twice what its load took (a step of the string table)",
                 T[i].name, need);
        ck(need >= 2 * T[i].took, what);
        snprintf(what, sizeof what, "%s: need %zu B below the rule before (%zu B)", T[i].name, need,
                 2 * T[i].plen + 4096);
        ck(need < 2 * T[i].plen + 4096, what);
    }
    // which part decides
    ck(tplmem_block(30000, 1000) == 2 * (30000 + 512), "a long manifest: the two copies of the rest decide");
    ck(tplmem_block(100, 100) == 8192 + 512, "a small template: no less than 8 KB for the compiler");
    // the edges
    ck(tplmem_check(174616, 31578, 20124, 4591, 15533, 0) == TPLMEM_OK, "largest block exactly the need: loads");
    ck(tplmem_check(174616, 31577, 20124, 4591, 15533, 0) == TPLMEM_BLOCK, "one octet less: refused for the block");
    ck(tplmem_check(158676, 100000, 20124, 4591, 15533, 0) == TPLMEM_OK, "free heap exactly the need: loads");
    ck(tplmem_check(158675, 100000, 20124, 4591, 15533, 0) == TPLMEM_TOTAL, "one octet less in all: refused");
    ck(tplmem_check(100000, 10000, 20124, 4591, 15533, 0) == TPLMEM_TOTAL, "both short: the total is named");
    // the check before a download completes counts the VM the device gives up (63848 B for C002 v6)
    ck(tplmem_check(110768, 20000, 20124, 4591, 15533, 63848) == TPLMEM_OK,
       "before the download completes: the VM given up makes up for the total and for a small block");
    ck(tplmem_check(127099, 20000, 20124, 4591, 15533, 31577) == TPLMEM_BLOCK,
       "a VM given up smaller than the block need does not make up for a small block");
    ck(tplmem_check(90000, 100000, 20124, 4591, 15533, 63848) == TPLMEM_TOTAL,
       "the VM given up counts into the total only as far as it goes");
    printf("tplmem checks: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
