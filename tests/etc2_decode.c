/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* ETC2 and EAC blocks built by hand, one per mode, against values worked out from the Khronos Data Format
   Specification's ETC2 chapter */
#include "gpu/texture/etc2.h"
#include <stdio.h>
#include <string.h>

static int failures;

/* a 64 bit block word as the block's bytes, big-endian */
static void put_be(u8* p, u64 v) {
    for (int i = 0; i < 8; i++) p[i] = (u8)(v >> (56 - 8 * i));
}
static void rgba(const u8* blk, int kind, u32 x, u32 y, int r, int g, int b, int a, const char* what) {
    u8 px[16 * 4];
    etc2_decode_image(blk, 4, 4, kind, px);
    const u8* p = px + (y * 4 + x) * 4;
    if (p[0] != r || p[1] != g || p[2] != b || p[3] != a) {
        fprintf(stderr, "FAIL: %s: (%u,%u) is %u,%u,%u,%u, expected %d,%d,%d,%d\n", what, x, y, p[0], p[1], p[2], p[3],
                r, g, b, a);
        failures++;
    }
}
static void r16(const u8* blk, int kind, u32 x, u32 y, u16 want, const char* what) {
    u16 px[16 * 2];
    etc2_decode_image(blk, 4, 4, kind, (u8*)px);
    u16 got = px[(y * 4 + x) * (etc2_pixel_bytes(kind) / 2)];
    if (got != want) {
        fprintf(stderr, "FAIL: %s: (%u,%u) is %u, expected %u\n", what, x, y, got, want);
        failures++;
    }
}

int main(void) {
    u8 blk[16];

    /* individual: R 0xA and 0x5, G 0, B 0xF; codewords 0 and 7; side by side (no flip) */
    put_be(blk, 0xA500FF1Cull << 32);
    rgba(blk, ETC2_RGB, 0, 0, 172, 2, 255, 255, "individual, index 0, left");
    rgba(blk, ETC2_RGB, 3, 3, 132, 47, 255, 255, "individual, index 0, right");
    put_be(blk, 0xA500FF1Cull << 32 | 0xFFFF0000u);
    rgba(blk, ETC2_RGB, 1, 2, 168, 0, 253, 255, "individual, index 2, left, clamped at 0");
    rgba(blk, ETC2_RGB, 2, 1, 38, 0, 208, 255, "individual, index 2, right");

    /* T: red overflows (31 + 1); C1 red 0b1101, C2 green 8, distance index 3 (16) */
    put_be(blk, 0xF9000807ull << 32 | 0xC000Au);
    rgba(blk, ETC2_RGB, 0, 0, 221, 0, 0, 255, "T, index 0: the first color");
    rgba(blk, ETC2_RGB, 0, 1, 16, 152, 16, 255, "T, index 1: the second plus the distance");
    rgba(blk, ETC2_RGB, 0, 2, 0, 136, 0, 255, "T, index 2: the second");
    rgba(blk, ETC2_RGB, 0, 3, 0, 120, 0, 255, "T, index 3: the second less the distance, clamped");

    /* planar: blue overflows (0 - 4); only the horizontal green is set, 127 */
    put_be(blk, 0x402FE000000ull);
    rgba(blk, ETC2_RGB, 0, 0, 0, 0, 0, 255, "planar at the origin");
    rgba(blk, ETC2_RGB, 1, 3, 0, 64, 0, 255, "planar, x 1");
    rgba(blk, ETC2_RGB, 3, 0, 0, 191, 0, 255, "planar, x 3");

    /* punch-through, opaque bit clear: index 2 is transparent, index 0 keeps the base, index 1 adds 8 */
    put_be(blk, 0x50000000ull << 32 | 0x10004u);
    rgba(blk, ETC2_RGB_PTA, 0, 0, 0, 0, 0, 0, "see-through, index 2");
    rgba(blk, ETC2_RGB_PTA, 0, 1, 82, 0, 0, 255, "see-through, index 0 unmodified");
    rgba(blk, ETC2_RGB_PTA, 0, 2, 90, 8, 8, 255, "see-through, index 1");

    /* RGBA: alpha base 200, multiplier 3, table 13, every index 7 (+9); then the individual block */
    put_be(blk, 0xC83DFFFFFFFFFFFFull);
    put_be(blk + 8, 0xA500FF1Cull << 32);
    rgba(blk, ETC2_RGBA, 0, 0, 172, 2, 255, 227, "RGBA: EAC alpha and ETC2 color");

    /* EAC R11: base 100, multiplier 2, table 0, every index 7 (+14): 100 * 8 + 4 + 14 * 2 * 8 = 1028 */
    put_be(blk, 0x6420FFFFFFFFFFFFull);
    r16(blk, EAC_R, 2, 2, 32912, "EAC R11 unsigned, to 16 bits");
    /* signed: base -128 reads as -127, multiplier 0, table 13, index 0 (-1): -1017 */
    put_be(blk, 0x800D000000000000ull);
    r16(blk, EAC_R_SNORM, 1, 0, 32961, "EAC R11 signed, to 16 bits");
    /* RG: the second block is the green channel */
    put_be(blk, 0x800D000000000000ull);
    put_be(blk + 8, 0x6420FFFFFFFFFFFFull);
    {
        u16 px[16 * 2];
        etc2_decode_image(blk, 4, 4, EAC_RG, (u8*)px);
        if (px[1] != 32912) {
            fprintf(stderr, "FAIL: EAC RG11: green is %u, expected 32912\n", px[1]);
            failures++;
        }
    }

    if (failures) return 1;
    puts("ETC2 individual, T, planar, punch-through and RGBA; EAC R11 and RG11, unsigned and signed: as specified");
    return 0;
}
