/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_NVN_DESCRIPTOR_H
#define SNX_NVN_DESCRIPTOR_H
#include "abi/cpu.h"
#include "gpu/nvn_texture.h"

typedef struct {
    u64 object, pool, texture, view, handle;
    u64 memory_pool;
    i64 memory_offset;
    u32 index, count;
    NvnTextureViewSelection view_selection;
    u8 view_known;
} NvnDescriptorCall;
typedef struct {
    u64 texture, view, texture_generation, pool_generation;
    u64 memory_pool, memory_pool_generation;
    NvnTextureViewSelection view_selection;
    u32 tic[8];
    u8 view_known;
    u8 tic_valid;
} NvnDescriptorSnapshot;

int nvn_descriptor_wants(const char* name);
void nvn_descriptor_before(const CPU* c, const char* name, NvnDescriptorCall* call);
void nvn_descriptor_after(const CPU* c, const char* name, const NvnDescriptorCall* call);
void nvn_descriptor_report(void);
/* 1 = live texture, 0 = missing pool/descriptor, -1 = stale texture. */
int nvn_descriptor_check(u64 pool, u64 handle);
/* Returns 1 for a live texture registration, 0 for a missing descriptor, -1 for a stale texture. */
int nvn_descriptor_snapshot(u64 pool, u64 handle, NvnDescriptorSnapshot* out);
/* Captured image storage/view metadata, checked against the registered backing pool generation. */
int nvn_descriptor_target_snapshot(u64 pool, u64 handle, NvnTextureTarget* out);
#endif
