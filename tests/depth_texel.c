/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Packed depth/stencil texels split by format: stencil low for G24R8 and Z24S8, depth low for X8Z24 and S8Z24 */
#include "nx/gpu/texture/depth.h"
#include <stdio.h>

static int failures;
static void check(int ok, const char* what) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

int main(void) {
    const u32 texel = 0xAB123456u;
    u8 stencil = 0xEE;
    check(depth24_unpack(0x29, texel, &stencil) == 0xAB1234 && stencil == 0x56, "Z24S8: stencil in the low byte");
    check(depth24_unpack(0x0E, texel, &stencil) == 0xAB1234 && stencil == 0x56, "G24R8 reads like Z24S8");
    check(depth24_unpack(0x2B, texel, &stencil) == 0x123456 && stencil == 0xAB, "S8Z24: depth in the low 24 bits");
    check(depth24_unpack(0x0D, texel, &stencil) == 0x123456 && stencil == 0xAB, "G8R24 reads like S8Z24");
    stencil = 0xEE;
    check(depth24_unpack(0x2A, texel, &stencil) == 0x123456 && stencil == 0, "X8Z24: no stencil");
    check(depth24_unpack(0x29, 0xFFFFFF00u, &stencil) == 0xFFFFFF && !stencil, "Z24S8 far plane");
    if (!failures) puts("Packed depth/stencil texels split by where each format keeps its depth");
    return failures ? 1 : 0;
}
