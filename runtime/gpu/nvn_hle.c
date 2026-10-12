/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gpu/nvn_hle.h"
#include "platform/log.h"
#include <string.h>

static u64 hle_texture_handle;

int nvn_hle_supported(const char* name) { return !strcmp(name, "nvnDeviceGetTextureHandle"); }

int nvn_hle_try(CPU* c, const char* name) {
    if (!nvn_hle_supported(name)) return 0;
    /* Both inspected SDKs pack texture and sampler IDs into a sign-extended word, then set bit 32. */
    u32 packed = (u32)c->x[1] | ((u32)c->x[2] << 20);
    c->x[8] = (u64)(i64)(i32)packed;
    c->x[0] = c->x[8] | 0x100000000ull;
    c->pc = c->x[30];
    __atomic_fetch_add(&hle_texture_handle, 1, __ATOMIC_RELAXED);
    return 1;
}

void nvn_hle_report(void) {
    LOG("nvn-hle: texture handle %llu\n", (unsigned long long)__atomic_load_n(&hle_texture_handle, __ATOMIC_RELAXED));
}
