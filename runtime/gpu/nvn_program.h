/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_NVN_PROGRAM_H
#define SNX_NVN_PROGRAM_H
#include "abi/cpu.h"

typedef struct {
    u64 program;
    u32 shader_count;
} NvnProgramCall;

int nvn_program_wants(const char* name);
void nvn_program_before(const CPU* c, const char* name, NvnProgramCall* call);
void nvn_program_after(const CPU* c, const char* name, const NvnProgramCall* call);
/* 1 = live configured program, 0 = unknown/finalized, -1 = live but no successful shader setup. */
int nvn_program_check(u64 program);
void nvn_program_report(void);
#endif
