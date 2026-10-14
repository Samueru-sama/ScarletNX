/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* snx_crc32 against the bit-at-a-time definition, both polynomials, every width */
#include "cpu.h"
#include <stdio.h>

static int failures;
static u32 reference(u32 acc, u64 val, int bytes, int castagnoli) {
    u32 poly = castagnoli ? 0x82F63B78u : 0xEDB88320u;
    int i, k;
    for (i = 0; i < bytes; i++) {
        acc ^= (u8)(val >> (i * 8));
        for (k = 0; k < 8; k++) acc = (acc >> 1) ^ ((acc & 1) ? poly : 0);
    }
    return acc;
}
static u64 next(u64* s) {
    *s ^= *s << 13;
    *s ^= *s >> 7;
    *s ^= *s << 17;
    return *s;
}

int main(void) {
    /* the standard check values of "123456789" */
    static const u32 check[2] = {0xCBF43926u, 0xE3069283u};
    static const int widths[4] = {1, 2, 4, 8};
    u64 seed = (u64)0x9E3779B9 << 32 | 0x7F4A7C15;
    unsigned checks = 2;
    const char* p;
    u32 acc, got, want;
    u64 val, used;
    int c, i, w;
    for (c = 0; c < 2; c++) {
        acc = 0xFFFFFFFFu;
        for (p = "123456789"; *p; p++) acc = snx_crc32(acc, (u8)*p, 1, c);
        if (~acc != check[c]) {
            fprintf(stderr, "%s check value: %08x, expected %08x\n", c ? "CRC32C" : "CRC32", ~acc, check[c]);
            failures++;
        }
    }
    /* bits above the width must not count */
    for (i = 0; i < 20000; i++)
        for (c = 0; c < 2; c++)
            for (w = 0; w < 4; w++) {
                acc = (u32)next(&seed);
                val = next(&seed);
                used = widths[w] == 8 ? val : val & (((u64)1 << (8 * widths[w])) - 1);
                got = snx_crc32(acc, val, widths[w], c);
                want = reference(acc, used, widths[w], c);
                checks++;
                if (got != want && failures++ < 8)
                    fprintf(stderr, "%s, %d bytes, acc %08x, value %08lx%08lx: %08x, expected %08x\n",
                            c ? "CRC32C" : "CRC32", widths[w], acc, (unsigned long)(val >> 32),
                            (unsigned long)(val & 0xFFFFFFFFu), got, want);
            }
    printf("snx_crc32: %u checks, %d failed\n", checks, failures);
    return failures != 0;
}
