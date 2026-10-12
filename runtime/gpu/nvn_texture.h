/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_NVN_TEXTURE_H
#define SNX_NVN_TEXTURE_H
#include "abi/cpu.h"

typedef struct {
    u64 texture, pool, offset;
    u64 gpu_address;
    u32 flags, target, width, height, depth, levels, format, samples;
    u32 storage_size;
    int size_known;
    int address_known;
    int valid;
} NvnTextureCall;

typedef struct {
    u64 generation, pool, pool_generation, offset;
    u64 gpu_address;
    u32 flags, target, width, height, depth, levels, format, samples;
    u32 storage_size;
    int address_known;
} NvnTextureSnapshot;

typedef struct {
    u8 flags; /* levels, layers, format, swizzle, depth mode, target */
    u32 base_level, level_count, first_layer, layer_count, format, target;
} NvnTextureViewSelection;

typedef struct {
    u64 texture, pool, gpu_address, texture_generation, pool_generation;
    u32 width, height, depth, levels, format, target, samples, flags;
    NvnTextureViewSelection view;
} NvnTextureTarget;

typedef enum {
    NVN_TARGET_RESOLVED = 1,
    NVN_TARGET_MISSING = 0,
    NVN_TARGET_STALE_POOL = -1,
    NVN_TARGET_NO_ADDRESS = -2,
    NVN_TARGET_BAD_VIEW = -3
} NvnTargetResult;

int nvn_texture_wants(const char* name);
void nvn_texture_before(const CPU* c, const char* name, NvnTextureCall* call);
void nvn_texture_after(const CPU* c, const char* name, const NvnTextureCall* call);
void nvn_texture_report(void);
int nvn_texture_snapshot(u64 texture, NvnTextureSnapshot* out);
NvnTargetResult nvn_texture_resolve_target(u64 texture, const NvnTextureViewSelection* view, NvnTextureTarget* out);
#endif
