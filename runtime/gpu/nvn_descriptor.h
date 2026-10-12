/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_NVN_DESCRIPTOR_H
#define SNX_NVN_DESCRIPTOR_H
#include "abi/cpu.h"

typedef struct {
    u64 object, pool, texture, view, handle;
    u32 index, count;
} NvnDescriptorCall;

int nvn_descriptor_wants(const char* name);
void nvn_descriptor_before(const CPU* c, const char* name, NvnDescriptorCall* call);
void nvn_descriptor_after(const CPU* c, const char* name, const NvnDescriptorCall* call);
void nvn_descriptor_report(void);
#endif
