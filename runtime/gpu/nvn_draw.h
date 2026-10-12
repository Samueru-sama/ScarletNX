/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_NVN_DRAW_H
#define SNX_NVN_DRAW_H
#include "abi/cpu.h"
#include "gpu/nvn_program.h"
#include "gpu/nvn_sampler_descriptor.h"
#include "gpu/nvn_descriptor.h"
#include "gpu/nvn_texture.h"

#define NVN_DRAW_VERTEX_SLOTS  16
#define NVN_DRAW_SHADER_STAGES 6
#define NVN_DRAW_UNIFORM_SLOTS 16
#define NVN_DRAW_TEXTURE_SLOTS 32
enum {
    NVN_DRAW_PREPARED = 1,
    NVN_DRAW_SHARED_TARGET = 2,
    NVN_DRAW_TARGET_NO_SURFACE = 4,
    NVN_DRAW_TARGET_FORMAT = 8,
    NVN_DRAW_TARGET_SIZE = 16,
    NVN_DRAW_TARGET_VIEW = 32
};
typedef struct {
    u64 address, size;
} NvnBufferRange;
typedef struct {
    u64 handle;
    NvnProgramSnapshot snapshot;
} NvnStageProgram;
typedef struct {
    u32 format, offset, stream;
    u8 enabled;
} NvnVertexAttrib;
typedef struct {
    u32 stride, divisor;
} NvnVertexStream;
typedef struct {
    u64 sequence;
    NvnStageProgram program[NVN_DRAW_SHADER_STAGES];
    u8 program_bound;
    NvnBufferRange vertex[NVN_DRAW_VERTEX_SLOTS];
    u32 vertex_attrib_raw[NVN_DRAW_VERTEX_SLOTS];
    u64 vertex_stream_raw[NVN_DRAW_VERTEX_SLOTS];
    NvnVertexAttrib vertex_attrib[NVN_DRAW_VERTEX_SLOTS];
    NvnVertexStream vertex_stream[NVN_DRAW_VERTEX_SLOTS];
    u8 vertex_attrib_count, vertex_stream_count;
    NvnBufferRange uniform[NVN_DRAW_SHADER_STAGES][NVN_DRAW_UNIFORM_SLOTS];
    u16 uniform_bound[NVN_DRAW_SHADER_STAGES];
    u64 texture[NVN_DRAW_SHADER_STAGES][NVN_DRAW_TEXTURE_SLOTS];
    NvnDescriptorSnapshot texture_descriptor[NVN_DRAW_SHADER_STAGES][NVN_DRAW_TEXTURE_SLOTS];
    NvnSamplerDescriptorSnapshot sampler_descriptor[NVN_DRAW_SHADER_STAGES][NVN_DRAW_TEXTURE_SLOTS];
    u32 texture_bound[NVN_DRAW_SHADER_STAGES];
    u32 texture_descriptor_valid[NVN_DRAW_SHADER_STAGES];
    u32 sampler_descriptor_valid[NVN_DRAW_SHADER_STAGES];
    NvnTextureTarget color_target[8], depth_target;
    u8 color_target_mask, depth_target_bound, target_complete;
    u64 index_address;
    u32 primitive, first, count, index_type, instance_count, base_instance;
    i32 base_vertex;
    u16 vertex_bound;
    u8 indexed, program_known, shader_refs_known;
} NvnDrawCommand;
#endif
