/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_NVN_POOL_H
#define SNX_NVN_POOL_H
#include "abi/cpu.h"

typedef struct {
    u64 pool, storage, size;
    u32 flags;
    int valid;
} NvnPoolCall;

int nvn_pool_wants(const char* name);
void nvn_pool_before(const CPU* c, const char* name, NvnPoolCall* call);
void nvn_pool_after(const CPU* c, const char* name, const NvnPoolCall* call);
void nvn_pool_report(void);
#endif
