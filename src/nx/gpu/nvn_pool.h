/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_NVN_POOL_H
#define SNX_NVN_POOL_H
#include "cpu/aarch64/cpu.h"

typedef struct {
    u64 pool, storage, size;
    u32 flags;
    int valid, legacy_layout;
} NvnPoolCall;

typedef struct {
    u64 size, generation, gpu_address, storage;
    int gpu_seen;
} NvnPoolSnapshot;

int nvn_pool_wants(const char* name);
void nvn_pool_before(const CPU* c, const char* name, NvnPoolCall* call);
void nvn_pool_after(const CPU* c, const char* name, const NvnPoolCall* call);
void nvn_pool_report(void);
int nvn_pool_snapshot(u64 pool, NvnPoolSnapshot* out);
/* Copies a bounded descriptor from the pool backing without forcing a GPU readback. Returns 2 for CPU storage,
   1 for a GPU mapping, or 0 when neither is readable or the pool generation changed. */
int nvn_pool_read_backing(u64 pool, u64 generation, u64 offset, void* dst, u64 size);
/* 0 = unavailable, 1 = learned the base, 2 = consistent, 3 = mismatch. */
int nvn_pool_observe_buffer_address(u64 pool, u64 generation, u64 offset, u64 address);
#endif
