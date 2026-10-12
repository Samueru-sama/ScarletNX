/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_NVN_DRAW_H
#define SNX_NVN_DRAW_H
#include "abi/cpu.h"
#include "gpu/nvn_program.h"

#define NVN_DRAW_VERTEX_SLOTS 16
typedef struct {
    u64 address, size;
} NvnBufferRange;
typedef struct {
    u64 program, program_generation;
    NvnShaderRef shaders[NVN_PROGRAM_MAX_SHADERS];
    u32 shader_count;
    NvnBufferRange vertex[NVN_DRAW_VERTEX_SLOTS];
    u64 index_address;
    u32 primitive, first, count, index_type, instance_count, base_instance;
    i32 base_vertex;
    u16 vertex_bound;
    u8 indexed, program_known, shader_refs_known;
} NvnDrawCommand;
#endif
