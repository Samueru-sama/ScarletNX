/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_NVN_DRAW_H
#define SNX_NVN_DRAW_H
#include "cpu/aarch64/cpu.h"
#include "gpu/nvn_program.h"
#include "gpu/nvn_sampler_descriptor.h"
#include "gpu/nvn_descriptor.h"
#include "gpu/nvn_texture.h"

#define NVN_DRAW_VERTEX_SLOTS   16
#define NVN_DRAW_SHADER_STAGES  6
#define NVN_DRAW_UNIFORM_SLOTS  16
#define NVN_DRAW_STORAGE_SLOTS  16
#define NVN_DRAW_TEXTURE_SLOTS  32
#define NVN_DRAW_IMAGE_SLOTS    8
#define NVN_DRAW_VIEWPORT_SLOTS 16
struct NvnStorageSeed;
typedef struct NvnUniformUpdate {
    struct NvnUniformUpdate* next; /* newest first; draw snapshots retain this head */
    u64 address;
    u32 size;
    u8 bytes[];
} NvnUniformUpdate;
enum {
    NVN_DRAW_PREPARED = 1,
    NVN_DRAW_SHARED_TARGET = 2,
    NVN_DRAW_TARGET_NO_SURFACE = 4,
    NVN_DRAW_TARGET_FORMAT = 8,
    NVN_DRAW_TARGET_SIZE = 16,
    NVN_DRAW_TARGET_VIEW = 32,
    NVN_DRAW_SAMPLER_READY = 64,
    NVN_DRAW_SAMPLER_MISSING = 128,
    NVN_DRAW_SAMPLER_FAILED = 256,
    NVN_DRAW_TEXTURE_HEADER_READY = 512,
    NVN_DRAW_TEXTURE_HEADER_MISSING = 1024,
    NVN_DRAW_TEXTURE_HEADER_UNSUPPORTED = 2048,
    NVN_DRAW_TEXTURE_IMAGE_READY = 4096,
    NVN_DRAW_TEXTURE_IMAGE_FAILED = 8192,
    NVN_DRAW_TEXTURE_IMAGE_DEPTH = 16384,
    NVN_DRAW_TEXTURE_IMAGE_MISSING = 32768,
    NVN_DRAW_TEXTURE_IMAGE_SEMANTIC = 65536,
    NVN_DRAW_ATTACHMENT_READY = 131072,
    NVN_DRAW_ATTACHMENT_FAILED = 262144,
    NVN_DRAW_SHADER_MODULE_READY = 524288,
    NVN_DRAW_SHADER_MODULE_FAILED = 1048576,
    NVN_DRAW_VERTEX_FORMAT_READY = 2097152,
    NVN_DRAW_VERTEX_FORMAT_FAILED = 4194304,
    NVN_DRAW_PIPELINE_READY = 8388608,
    NVN_DRAW_PIPELINE_FAILED = 16777216,
    NVN_DRAW_TEXTURE_IMAGE_STALE = 33554432,
    NVN_DRAW_ZERO_BIND_TEXTURELESS = 67108864,
    NVN_DRAW_ZERO_BIND_TEXTURED = 134217728,
    NVN_DRAW_ZERO_BIND_ALL_BINDLESS = 268435456,
    NVN_DRAW_TEXTURE_UNUSED = 536870912,
    NVN_DRAW_TEXTURE_SLOTS_MAPPED = 1073741824
};
#define NVN_DRAW_IMAGE_STORAGE_READY 0x80000000u
typedef struct {
    u64 address, size;
} NvnBufferRange;
typedef struct {
    NvnBufferRange range;
    u32 cbuf_word; /* encoded MME argument; the offset is validated before use */
} NvnStorageBinding;
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
    u64 queue_key;
    u64 source_list, source_visit;
    u64 source_command_low, source_command_high;
    u64 last_unmodeled_write; /* earlier operations whose image effects are not in the semantic stream */
    NvnStageProgram program[NVN_DRAW_SHADER_STAGES];
    u8 program_bound;
    NvnBufferRange vertex[NVN_DRAW_VERTEX_SLOTS];
    u32 vertex_attrib_raw[NVN_DRAW_VERTEX_SLOTS];
    u64 vertex_stream_raw[NVN_DRAW_VERTEX_SLOTS];
    NvnVertexAttrib vertex_attrib[NVN_DRAW_VERTEX_SLOTS];
    NvnVertexStream vertex_stream[NVN_DRAW_VERTEX_SLOTS];
    u8 vertex_attrib_count, vertex_stream_count;
    NvnBufferRange uniform[NVN_DRAW_SHADER_STAGES][NVN_DRAW_UNIFORM_SLOTS];
    const struct NvnStorageSeed* uniform_seed[2][NVN_DRAW_UNIFORM_SLOTS];
    const NvnUniformUpdate* uniform_updates;
    u8 uniform_updates_complete;
    u16 uniform_bound[NVN_DRAW_SHADER_STAGES];
    NvnStorageBinding storage[2][NVN_DRAW_STORAGE_SLOTS]; /* vertex and fragment */
    u16 storage_bound[2];
    u16 storage_known[2];
    u64 texture[NVN_DRAW_SHADER_STAGES][NVN_DRAW_TEXTURE_SLOTS];
    NvnDescriptorSnapshot texture_descriptor[NVN_DRAW_SHADER_STAGES][NVN_DRAW_TEXTURE_SLOTS];
    NvnSamplerDescriptorSnapshot sampler_descriptor[NVN_DRAW_SHADER_STAGES][NVN_DRAW_TEXTURE_SLOTS];
    u32 texture_bound[NVN_DRAW_SHADER_STAGES];
    u32 texture_descriptor_valid[NVN_DRAW_SHADER_STAGES];
    u32 sampler_descriptor_valid[NVN_DRAW_SHADER_STAGES];
    u64 image[NVN_DRAW_SHADER_STAGES][NVN_DRAW_IMAGE_SLOTS];
    NvnDescriptorSnapshot image_descriptor[NVN_DRAW_SHADER_STAGES][NVN_DRAW_IMAGE_SLOTS];
    NvnTextureTarget image_target[NVN_DRAW_SHADER_STAGES][NVN_DRAW_IMAGE_SLOTS];
    u8 image_bound[NVN_DRAW_SHADER_STAGES];
    u8 image_descriptor_valid[NVN_DRAW_SHADER_STAGES];
    u8 image_target_valid[NVN_DRAW_SHADER_STAGES];
    NvnTextureTarget color_target[8], depth_target;
    u8 color_target_mask, depth_target_bound, target_complete;
    u8 color_initial_zero; /* opt-in pre-submit zero-backing candidate */
    u8 write_set_scoped; /* replay proved all graphics writes are in bound ranges */
    u32 channel_mask;
    u8 channel_mask_known;
    u32 color_state;
    u8 color_state_known, blend_state_known;
    u32 blend_state[8][2];
    u32 polygon_state;
    u8 polygon_state_known;
    u8 window_flip_state; /* 0 unknown, 1 no flip, 2 flip, 3 conflicting writes */
    u32 depth_stencil_state[2];
    u8 depth_stencil_state_known;
    u8 depth_mode, depth_mode_known;
    u8 clip_enable, clip_enable_known;
    u32 multisample_state[6];
    u8 multisample_state_known;
    u32 viewport[NVN_DRAW_VIEWPORT_SLOTS][4];
    u32 scissor[NVN_DRAW_VIEWPORT_SLOTS][4];
    u32 depth_range[NVN_DRAW_VIEWPORT_SLOTS][2];
    u16 viewport_known, scissor_known, depth_range_known;
    u32 blend_color[4];
    u8 blend_color_known;
    u32 stencil_value_mask[2], stencil_write_mask[2], stencil_ref[2];
    u8 stencil_value_mask_known, stencil_write_mask_known, stencil_ref_known;
    u32 line_width, point_size, alpha_ref;
    u32 polygon_offset[3], depth_bounds[2], sample_mask;
    u8 line_width_known, point_size_known, alpha_ref_known, polygon_offset_known, depth_bounds_known;
    u8 depth_bounds_enabled, sample_mask_known, rasterizer_discard_known, rasterizer_discard;
    u8 depth_clamp_known, depth_clamp;
    u64 index_address;
    u32 primitive, first, count, index_type, instance_count, base_instance;
    i32 base_vertex;
    u16 vertex_bound;
    u8 indexed, program_known, shader_refs_known;
    u8 load_textures, prepare_attachments, materialize_shaders, prepare_pipeline;
} NvnDrawCommand;
#endif
