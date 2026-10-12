/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_NVN_SHADER_H
#define SNX_NVN_SHADER_H
#include "abi/cpu.h"

/* The same 64 KiB limit used by the Maxwell renderer's shader reader. */
#define NVN_SHADER_CODE_MAX 0x10000
typedef struct {
    u32 blob_size, sph_word;
    int stage; /* ST_* from shader.h, or -1 for an unrecognized header */
} NvnShaderMeta;

/* GPU-thread only. Reads the guest NVN code blob and copies the SPH plus Maxwell instructions into code. */
int nvn_shader_load(u64 address, u8 code[NVN_SHADER_CODE_MAX], NvnShaderMeta* out);
#endif
