/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gpu/nvn_control.h"

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
