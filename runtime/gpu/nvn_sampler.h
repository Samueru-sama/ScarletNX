/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_NVN_SAMPLER_H
#define SNX_NVN_SAMPLER_H
#include "abi/cpu.h"

typedef struct {
    u64 sampler;
} NvnSamplerCall;
typedef struct {
    u64 generation;
} NvnSamplerSnapshot;

int nvn_sampler_wants(const char* name);
void nvn_sampler_before(const CPU* c, NvnSamplerCall* call);
void nvn_sampler_after(const CPU* c, const char* name, const NvnSamplerCall* call);
int nvn_sampler_snapshot(u64 sampler, NvnSamplerSnapshot* out);
void nvn_sampler_report(void);
#endif
