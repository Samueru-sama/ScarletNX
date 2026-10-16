/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/gpu/maxwell/write_pages.h"
#include <stdlib.h>

#define LEAF_PAGES 4096u
struct WritePageLeaf {
    u64 max;
    u64 gen[LEAF_PAGES];
};
void write_pages_stamp(WritePages* t, u64 a, u64 size, u64 gen) {
    u64 p, e;
    if (!size) return;
    if (a + size - 1 < a) {
        t->far = gen;
        return;
    }
    for (p = a >> 12, e = (a + size - 1) >> 12; p <= e; p++) {
        WritePageLeaf** l = &t->leaf[(p >> 12) & 0xFFFF];
        if (p >> 28 || (!*l && !(*l = calloc(1, sizeof(WritePageLeaf))))) {
            t->far = gen;
            return;
        }
        (*l)->gen[p & (LEAF_PAGES - 1)] = (*l)->max = gen;
    }
}
int write_pages_since(const WritePages* t, u64 since, u64 a, u64 size) {
    u64 p, e;
    if (t->far > since) return 1;
    if (!size) return 0;
    e = a + size - 1 < a ? ~0ull >> 12 : (a + size - 1) >> 12; /* pages past 2^40 were only ever stamped as far */
    for (p = a >> 12; p <= e && !(p >> 28);) {
        u64 end = (p | (LEAF_PAGES - 1)) < e ? p | (LEAF_PAGES - 1) : e;
        const WritePageLeaf* l = t->leaf[p >> 12];
        u64 q;
        if (l && l->max > since)
            for (q = p; q <= end; q++)
                if (l->gen[q & (LEAF_PAGES - 1)] > since) return 1;
        p = end + 1;
    }
    return 0;
}
