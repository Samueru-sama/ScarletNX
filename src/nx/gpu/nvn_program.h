/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_NVN_PROGRAM_H
#define SNX_NVN_PROGRAM_H
#include "cpu/aarch64/cpu.h"

#define NVN_PROGRAM_MAX_SHADERS 8
typedef struct {
    u64 code_candidate, control_candidate, extension;
} NvnShaderRef;
typedef struct {
    u64 program;
    u32 shader_count;
    int refs_valid;
    NvnShaderRef refs[NVN_PROGRAM_MAX_SHADERS];
} NvnProgramCall;
typedef struct {
    u64 generation;
    u32 shader_count;
    int refs_known;
    NvnShaderRef refs[NVN_PROGRAM_MAX_SHADERS];
} NvnProgramSnapshot;

int nvn_program_wants(const char* name);
void nvn_program_enable_translation(void);
void nvn_program_translate_async(u64 code_address);
void nvn_program_before(const CPU* c, const char* name, NvnProgramCall* call);
void nvn_program_after(const CPU* c, const char* name, const NvnProgramCall* call);
/* 1 = live configured program, 0 = unknown/finalized, -1 = live but no successful shader setup. */
int nvn_program_check(u64 program);
int nvn_program_snapshot(u64 program, NvnProgramSnapshot* out);
/* True only for a current compute blob with a fully bounded NVN storage write set. */
int nvn_program_compute_write_slots(u64 code_address, u64 control_address, u16* slots);
void nvn_program_report(void);
#endif
