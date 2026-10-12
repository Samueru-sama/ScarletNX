/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_NVN_SAMPLER_DESCRIPTOR_H
#define SNX_NVN_SAMPLER_DESCRIPTOR_H
#include "abi/cpu.h"

typedef struct {
    u64 object, sampler;
    u32 index, count;
} NvnSamplerDescriptorCall;

int nvn_sampler_descriptor_wants(const char* name);
void nvn_sampler_descriptor_before(const CPU* c, const char* name, NvnSamplerDescriptorCall* call);
void nvn_sampler_descriptor_after(const CPU* c, const char* name, const NvnSamplerDescriptorCall* call);
/* 1 = live registration, 0 = missing pool/registration, -1 = stale sampler. */
int nvn_sampler_descriptor_check(u64 pool, u64 handle);
void nvn_sampler_descriptor_report(void);
#endif
