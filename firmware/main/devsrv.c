// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Dirk Tostmann
// Dummy of the KNX device server: one RAM record per virtual device, allocated one by one like the real thing would.
// The layout is an assumption for the measurement, not the final design: address and association table with 64
// entries each, 16 group objects with up to 6 value bytes, a 64-byte parameter copy, LSM states.
#include <stdio.h>
#include <stdlib.h>
#include "app.h"

typedef struct {
    uint16_t ia;
    uint8_t lsm[4];
    uint16_t addr_tab[64];
    uint16_t assoc_tab[64];
    struct {
        uint8_t flags, len, val[6];
    } ko[16];
    uint8_t params[64];
} vdev_t;

#define DEV_MAX 64
static vdev_t *s_dev[DEV_MAX];
static int s_n;

int devsrv_resize(int n)
{
    if (n > DEV_MAX) {
        n = DEV_MAX;
    }
    while (s_n < n) {
        vdev_t *d = calloc(1, sizeof *d);
        if (!d) {
            break;
        }
        d->ia = (uint16_t)(0xFF00 | (s_n + 1));   // 15.15.(n+1)
        s_dev[s_n++] = d;
    }
    while (s_n > n) {
        free(s_dev[--s_n]);
        s_dev[s_n] = NULL;
    }
    return s_n;
}

void devsrv_print(void)
{
    printf("devsrv: %d virtual devices x %u B = %u B\n", s_n, (unsigned)sizeof(vdev_t),
           (unsigned)(s_n * sizeof(vdev_t)));
}
