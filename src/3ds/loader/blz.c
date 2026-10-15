/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Format/algorithm reference: Citra/Azahar ncch_container.cpp LZSS helpers,
   Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project,
   GPL-3.0-or-later. This variant requires an exact bounded expansion. */
#include "internal.h"
#include <string.h>

bool snx_ctr_blz(const unsigned char* input, size_t size, unsigned char* output, size_t expanded) {
    if (!input || !output || size < 8 || expanded < size || expanded > CTR_CODE_LIMIT) return false;
    uint32_t footer = ctr_le32(input + size - 8);
    size_t top = footer >> 24, bottom = footer & 0xffffff;
    if (top < 8 || top > bottom || bottom > size || expanded - size != ctr_le32(input + size - 4)) return false;
    size_t index = size - top, stop = size - bottom, out = expanded;
    memcpy(output, input, size);
    while (index > stop && out > stop) {
        unsigned control = input[--index];
        for (unsigned bit = 0; bit < 8 && out > stop; ++bit, control <<= 1) {
            if (control & 0x80) {
                if (index - stop < 2) return false;
                index -= 2;
                unsigned encoded = ctr_le16(input + index);
                size_t count = (encoded >> 12) + 3, distance = (encoded & 4095) + 3;
                if (count > out - stop) return false;
                while (count--) {
                    --out;
                    if (distance >= expanded - out) return false;
                    output[out] = output[out + distance];
                }
            } else {
                if (index == stop) return false;
                output[--out] = input[--index];
            }
            if (index == stop) break;
        }
    }
    return index == stop && out == stop;
}
