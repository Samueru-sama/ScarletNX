/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_NVN_SAMPLER_H
#define SNX_NVN_SAMPLER_H
#include "abi/cpu.h"

typedef struct {
    u64 sampler, builder;
    u32 a, b, d;
    f32 f0, f1;
    u32 border_color[4];
    u8 border_valid;
} NvnSamplerCall;
enum {
    NVN_SAMPLER_FILTER_KNOWN = 1,
    NVN_SAMPLER_WRAP_KNOWN = 2,
    NVN_SAMPLER_LOD_CLAMP_KNOWN = 4,
    NVN_SAMPLER_LOD_BIAS_KNOWN = 8,
    NVN_SAMPLER_COMPARE_KNOWN = 16,
    NVN_SAMPLER_ANISOTROPY_KNOWN = 32,
    NVN_SAMPLER_REDUCTION_KNOWN = 64,
    NVN_SAMPLER_LOD_SNAP_KNOWN = 128,
    NVN_SAMPLER_BORDER_KNOWN = 256
};
typedef struct {
    u32 min_filter, mag_filter, wrap_s, wrap_t, wrap_r, compare_mode, compare_func, reduction, known;
    u32 border_color[4], border_type;
    f32 lod_min, lod_max, lod_bias, anisotropy, lod_snap;
} NvnSamplerState;
typedef struct {
    u64 generation;
    NvnSamplerState state;
} NvnSamplerSnapshot;

int nvn_sampler_wants(const char* name);
void nvn_sampler_before(const CPU* c, const char* name, NvnSamplerCall* call);
void nvn_sampler_after(const CPU* c, const char* name, const NvnSamplerCall* call);
int nvn_sampler_snapshot(u64 sampler, NvnSamplerSnapshot* out);
void nvn_sampler_report(void);
#endif
