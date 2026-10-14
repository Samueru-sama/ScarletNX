/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_NVN_CONTROL_H
#define SNX_NVN_CONTROL_H
#include "cpu/aarch64/cpu.h"
#define NVN_CONTROL_SENTINEL 0xffffffffffffff01ull

/* Find a submitted GPFIFO entry in a bounded NVN control-memory record.
   The record begins with the observed list sentinel; a second sentinel ends it.
   The record value is an opaque control field, not the submitted word count.
   Return the 64-bit word offset and record value when requested. */
int nvn_control_find_gpu_entry(const u64* control, u32 qwords, u64 gpu_va,
                               u64 cpu_address, u32* offset, u32* record_value);

typedef struct {
    u64 index_address;
    u32 index_type, primitive, count, instances;
    u32 macro_args[6]; /* preserve SDK-specific arguments until their effects are proven */
} NvnControlMacro38Draw;

/* Decode the observed 12-word secondary-list indexed draw. This extracts draw
   arguments but does not establish the surrounding render state or execute it. */
int nvn_control_macro38_draw(const u32* words, u32 count, NvnControlMacro38Draw* out);

typedef struct {
    u32 selector, code_offset;
    u32 macro_args[5];
} NvnControlProgramBind;

/* Extract complete MME macro-12 program binds from a validated GPFIFO entry.
   The observed selector 1 writes VertexB's code offset and 5 writes fragment's.
   Returns zero for malformed packets or insufficient output capacity. */
int nvn_control_program_binds(const u32* words, u32 count, NvnControlProgramBind* out,
                              u32 capacity, u32* bind_count);

/* A raw draw's shader inputs, captured from submitted 3D register writes.
   The probe translates these inputs but does not submit a draw. */
typedef struct {
    u64 code[2];
    u64 cbuf_address[2][18];
    u32 cbuf_size[2][18];
    u32 cbuf_known[2];
    u32 attr_int, attr_sint;
    u32 raw_vertex_attrib[32], raw_vertex_attrib_written;
    u32 clip_enable;
    u64 tic_base, tsc_base;
    u32 texture_cbuf_bank, sampler_index_mode;
    u8 texture_registers_known;
    const struct NvnControlTextureDescriptor* texture_descriptors;
    u32 texture_descriptor_mask;
} NvnControlShaderProbe;
typedef struct NvnControlTextureDescriptor {
    u32 handle, tic[8], tsc[8];
} NvnControlTextureDescriptor;
typedef struct {
    u32 ready;
    u32 cbuf_mask[2], cbuf_bound[2], texture_count[2], texture_slots[2], image_count[2];
    u8 texture_slots_known[2];
    u32 texture_handles_mapped[2], texture_descriptors_mapped[2];
    u32 texture_snapshots_available[2], texture_snapshots_match[2], texture_shapes_supported[2];
    u32 first_texture_handle;
    u32 vertex_input_mask;
    u32 vertex_attrib_known, vertex_stream_ready, vertex_format_ready;
} NvnControlShaderProbeResult;
#endif
