/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gpu/nvn_command_gap.h"

int nvn_command_gap_indexed_draw(const u32* words, u32 length, NvnGapIndexedDraw* out) {
    if (!words || !out || length < 6 || length > 16384) return 0;
    for (u32 i = 0; i < length;) {
        u32 header = words[i];
        u32 mode = header >> 29, sub = (header >> 13) & 7, method = header & 0xfff;
        u32 count = (header >> 16) & 0x1fff;
        if (mode == 4) {
            i++;
            continue;
        }
        if ((mode != 1 && mode != 3 && mode != 5) || !count || count >= length - i) return 0;
        if (mode == 5 && sub == 0 && method == 0xe38 && count == 5 && i + 6 == length) {
            u32 primitive = words[i + 1], high = words[i + 2], low = words[i + 3];
            u32 index_type = words[i + 4], vertices = words[i + 5];
            if (primitive > 6 || high >= 256 || index_type > 2 || !vertices || vertices > (1u << 24)) return 0;
            *out = (NvnGapIndexedDraw){((u64)high << 32) | low, primitive, index_type, vertices};
            return 1;
        }
        i += count + 1;
    }
    return 0;
}
