/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include <assert.h>
#include "nx/gpu/vk/backing.h"
u64 gmmu_translate(u64 a) {
    if (a >= 0x100000 && a < 0x200000) return a - 0xF0000;
    if (a >= 0x300000 && a < 0x400000) return a - 0x2F0000;
    if (a == 0x500000) return 0x900000;
    if (a == 0x501000) return 0x10000;
    return 0;
}
int main(void) {
    assert(vk_backing_overlap(0x100000, 16, 0x300000, 16));
    assert(vk_backing_overlap(0x100000, 4096, 0x500000, 8192));
    assert(!vk_backing_overlap(0x100000, 4096, 0x500000, 4096));
    assert(!vk_backing_overlap(0x700000, 4096, 0x800000, 4096));
    assert(!vk_backing_overlap(0x100000, 4096, 0x301000, 4096));
    assert(vk_backing_overlap(0x100000, 65 * 4096, 0x300000, 65 * 4096));
    assert(vk_backing_overlap(0x100FFF, 2, 0x301000, 1));
    assert(!vk_backing_overlap(0x100000, 0, 0x100000, 16));
    return 0;
}
