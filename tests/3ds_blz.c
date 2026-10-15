/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "loader/ctr_loader.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    /* Reverse stream: three literals, 18+18+18+7 backreference bytes. */
    unsigned char packed[] = {0, 0x40, 0, 0xf0, 0, 0xf0, 0, 0xf0, 'A', 'A', 'A', 0x1e,
                              20, 0, 0, 8, 44, 0, 0, 0};
    unsigned char output[64];
    assert(snx_ctr_blz(packed, sizeof packed, output, sizeof output));
    for (size_t i = 0; i < sizeof output; ++i) assert(output[i] == 'A');
    assert(!snx_ctr_blz(packed, 7, output, sizeof output));
    assert(!snx_ctr_blz(packed, sizeof packed, output, sizeof output - 1));
    packed[12] = 255;
    assert(!snx_ctr_blz(packed, sizeof packed, output, sizeof output));
    packed[12] = 20; packed[15] = 7;
    assert(!snx_ctr_blz(packed, sizeof packed, output, sizeof output));
    packed[15] = 8; packed[11] = 0xff;
    assert(!snx_ctr_blz(packed, sizeof packed, output, sizeof output));
    packed[11] = 0x1e; packed[0] = 255;
    assert(!snx_ctr_blz(packed, sizeof packed, output, sizeof output));
    assert(!snx_ctr_blz(NULL, sizeof packed, output, sizeof output));
    puts("3DS bounded backward LZSS: exact expansion and malformed streams passed");
    return 0;
}
