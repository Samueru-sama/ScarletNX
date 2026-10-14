/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gpu/nvn_control.h"
#include <string.h>

int nvn_control_find_gpu_entry(const u64* control, u32 qwords, u64 gpu_va,
                               u64 cpu_address, u32* offset, u32* record_value) {
    if (!control || qwords < 4 || qwords > 8192 || control[0] != NVN_CONTROL_SENTINEL ||
        !gpu_va || (gpu_va & 3) || !cpu_address)
        return 0;
    for (u32 i = 1; i + 2 < qwords; i++) {
        if (control[i] == NVN_CONTROL_SENTINEL) break;
        if ((control[i] >> 32) != 3 || !(u32)control[i] ||
            control[i + 1] == NVN_CONTROL_SENTINEL || control[i + 2] == NVN_CONTROL_SENTINEL ||
            control[i + 1] != gpu_va || control[i + 2] != cpu_address)
            continue;
        if (offset) *offset = i;
        if (record_value) *record_value = (u32)control[i];
        return 1;
    }
    return 0;
}

static int packet_header(u32 word, u32 mode, u32 method, u32 count) {
    return (word >> 29) == mode && ((word >> 13) & 7) == 0 &&
           (word & 0xfff) == method && ((word >> 16) & 0x1fff) == count;
}

int nvn_control_macro38_draw(const u32* words, u32 count, NvnControlMacro38Draw* out) {
    if (!words || !out || count != 12 ||
        !packet_header(words[0], 4, 0xe20, (words[0] >> 16) & 0x1fff) ||
        !packet_header(words[1], 4, 0x5f6, (words[1] >> 16) & 0x1fff) ||
        !packet_header(words[2], 1, 0x5f2, 2) ||
        !packet_header(words[5], 5, 0xe4c, 6)) return 0;
    u32 type = (words[1] >> 16) & 0x1fff;
    if (type > 2 || words[3] >= 256 || !words[7] || !words[8] || words[6] > 9) return 0;
    NvnControlMacro38Draw draw = {.index_address = ((u64)words[3] << 32) | words[4],
                                  .index_type = type, .primitive = words[6],
                                  .count = words[7], .instances = words[8]};
    memcpy(draw.macro_args, &words[6], sizeof draw.macro_args);
    *out = draw;
    return 1;
}
