/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Reconcile a shader's mapped output with guest memory. The host may have written the guest range while the GPU was
   running: shader-changed bytes land only where the guest still holds the value the shader originally read. */
#include <stdlib.h>
#include <string.h>
#include "gpu/gpu.h"
#include "runtime/platform/log.h"
#include "gpu/vk/vk.h"

void vk_write_changed(u64 addr, const u8* res, const u8* org, u32 size) {
    u8* dst = gmem_ptr(addr, size);
    u8* tmp = NULL;
    const u8* cur = dst;
    int changed = 0;
    if (!cur) {
        tmp = malloc(size);
        if (!tmp) fatal("Out of memory landing a shader storage buffer");
        gmem_read(addr, tmp, size);
        cur = tmp;
    }
    for (u32 i = 0; i < size;) {
        if (i + 8 <= size && !memcmp(res + i, org + i, 8)) {
            i += 8;
            continue;
        }
        if (res[i] == org[i] || cur[i] != org[i]) {
            i++;
            continue;
        }
        u32 s = i;
        while (i < size && res[i] != org[i] && cur[i] == org[i]) i++;
        if (dst)
            memcpy(dst + s, res + s, i - s);
        else
            gmem_write(addr + s, res + s, i - s);
        changed = 1;
    }
    free(tmp);
    if (changed) gpu_mem_written_shader(addr, size); /* other GPU virtual addresses can alias these guest pages */
}
