/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gpu/nvn_control.h"
#include <assert.h>

int main(void) {
    const u64 control[] = {
        NVN_CONTROL_SENTINEL, 0, 0x100000003ull, 0x5d4e71a8cull, 0x1113d3a8cull,
        0x300000005ull, 0x658991ba4ull, 0x359bcaba4ull,
        0x300000005ull, 0x658991b74ull, 0x359bcab74ull,
        NVN_CONTROL_SENTINEL, 0x30000000cull, 0x658991ebcull, 0x359bcaebcull
    };
    u32 offset = 0;
    u32 record_value = 0;
    assert(nvn_control_find_gpu_entry(control, 15, 0x658991b74ull, 0x359bcab74ull,
                                      &offset, &record_value));
    assert(offset == 8);
    assert(record_value == 5);
    assert(!nvn_control_find_gpu_entry(control, 15, 0x658991b74ull, 0x359bcab75ull, NULL, NULL));
    const u64 wrong_record[] = {NVN_CONTROL_SENTINEL, 0x200000005ull,
                                0x658991b74ull, 0x359bcab74ull};
    assert(!nvn_control_find_gpu_entry(wrong_record, 4, 0x658991b74ull,
                                       0x359bcab74ull, NULL, NULL));
    assert(!nvn_control_find_gpu_entry(control, 15, 0x658991ebcull, 0x359bcaebcull, NULL, NULL));
    assert(!nvn_control_find_gpu_entry(control + 1, 14, 0x658991b74ull, 0x359bcab74ull, NULL, NULL));
    assert(!nvn_control_find_gpu_entry(control, 3, 0x658991b74ull, 0x359bcab74ull, NULL, NULL));
    assert(!nvn_control_find_gpu_entry(control, 15, 0x658991b75ull, 0x359bcab74ull, NULL, NULL));
    return 0;
}
