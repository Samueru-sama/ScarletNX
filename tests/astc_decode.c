/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gpu/texture/astc.h"
#include <stdio.h>
#include <string.h>

static int check(const u8* block, const u8 expected[4]) {
    u8 pixels[4 * 4 * 4];
    astc_decode_image(block, 4, 4, 4, 4, pixels, 0);
    for (int i = 0; i < 16; i++)
        if (memcmp(pixels + i * 4, expected, 4)) return 0;
    return 1;
}
/* a 2D void extent's four 13-bit S and T bounds, bits 12 to 63 */
static void set_extent(u8 block[16], u32 s0, u32 s1, u32 t0, u32 t1) {
    u64 low = 0, bounds = (u64)s0 | (u64)s1 << 13 | (u64)t0 << 26 | (u64)t1 << 39;
    for (int k = 0; k < 8; k++) low |= (u64)block[k] << (8 * k);
    low = (low & 0xFFF) | bounds << 12;
    for (int k = 0; k < 8; k++) block[k] = (u8)(low >> (8 * k));
}

int main(void) {
    /* LDR void extent: mode bits 0..8 = 0x1fc, bit 9 = 0, reserved bits 10..11 = 11, then an all-ones extent (no
       extent). RGBA16 endpoints occupy the last eight bytes. The zero alpha masks the solid color. */
    u8 block[16] = {0xfc, 0xfd, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x80, 0xff, 0xff, 0x00, 0x00};
    const u8 transparent[4] = {0, 128, 255, 0};
    const u8 invalid[4] = {255, 0, 255, 255};
    if (!check(block, transparent)) {
        fputs("ASTC LDR void extent did not preserve transparent color\n", stderr);
        return 1;
    }
    block[1] &= ~(1u << 2); /* reserved bit 10 must be one */
    if (!check(block, invalid)) {
        fputs("ASTC void extent with reserved bit 10 clear was accepted\n", stderr);
        return 1;
    }
    block[1] |= 1u << 2;
    block[1] &= ~(1u << 3); /* reserved bit 11 must be one */
    if (!check(block, invalid)) {
        fputs("ASTC void extent with reserved bit 11 clear was accepted\n", stderr);
        return 1;
    }
    block[1] |= 1u << 3;
    block[1] |= 1u << 1; /* bit 9 selects unsupported HDR endpoints */
    if (!check(block, invalid)) {
        fputs("ASTC HDR void extent was accepted as LDR\n", stderr);
        return 1;
    }
    block[1] &= ~(1u << 1);

    /* an extent needs each low bound below its high one, unless all four are ones (astcenc's and the ASTC spec's
       rule); otherwise the block is an error */
    static const u32 extents[][5] = {
        {0x0100, 0x0200, 0x0300, 0x0400, 1}, {0x0200, 0x0200, 0x0300, 0x0400, 0}, {0x0300, 0x0200, 0x0300, 0x0400, 0},
        {0x0100, 0x0200, 0x0400, 0x0400, 0}, {0x1FFF, 0x1FFF, 0x1FFF, 0x1FFE, 0}, {0x1FF0, 0x1FFF, 0x1FFF, 0x1FFF, 0},
    };
    for (size_t i = 0; i < sizeof(extents) / sizeof(extents[0]); i++) {
        u8 b[16];
        memcpy(b, block, sizeof b);
        set_extent(b, extents[i][0], extents[i][1], extents[i][2], extents[i][3]);
        if (!check(b, extents[i][4] ? transparent : invalid)) {
            fprintf(stderr, "ASTC void extent %04x-%04x x %04x-%04x: %s\n", extents[i][0], extents[i][1], extents[i][2],
                    extents[i][3], extents[i][4] ? "rejected" : "accepted");
            return 1;
        }
    }

    puts("PASS ASTC void extent color, bounds and invalid modes");
    return 0;
}
