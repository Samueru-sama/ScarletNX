/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_NVN_BUFFER_H
#define SNX_NVN_BUFFER_H
#include "cpu/aarch64/cpu.h"

typedef struct {
    u64 buffer, pool, offset, size;
    int valid;
} NvnBufferCall;

int nvn_buffer_wants(const char* name);
void nvn_buffer_before(const CPU* c, const char* name, NvnBufferCall* call);
void nvn_buffer_after(const CPU* c, const char* name, const NvnBufferCall* call);
void nvn_buffer_report(void);
#endif
