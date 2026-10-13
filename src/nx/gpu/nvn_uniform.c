/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gpu/nvn_uniform.h"
#include <stdlib.h>
#include <string.h>

int nvn_uniform_updates_apply(const NvnUniformUpdate* head, u64 address, u32 size, u8* data) {
    if (!data || !size || address > UINT64_MAX - size) return 0;
    u64 end = address + size;
    u32 count = 0;
    for (const NvnUniformUpdate* update = head; update; update = update->next) {
        if (!update->size || update->address > UINT64_MAX - update->size) return 0;
        if (update->address < end && address < update->address + update->size) {
            if (++count > 16384) return 0;
        }
    }
    if (!count) return 1;
    const NvnUniformUpdate** ordered = malloc((size_t)count * sizeof *ordered);
    if (!ordered) return 0;
    u32 index = 0;
    for (const NvnUniformUpdate* update = head; update; update = update->next)
        if (update->address < end && address < update->address + update->size)
            ordered[index++] = update;
    for (u32 i = count; i > 0; i--) {
        const NvnUniformUpdate* update = ordered[i - 1];
        u64 start = address > update->address ? address : update->address;
        u64 limit = end < update->address + update->size ? end : update->address + update->size;
        memcpy(data + (start - address), update->bytes + (start - update->address), (size_t)(limit - start));
    }
    free(ordered);
    return 1;
}
