/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_NVN_COMMAND_GAP_H
#define SNX_NVN_COMMAND_GAP_H
#include "cpu/aarch64/cpu.h"

typedef struct {
    u64 index_address;
    u32 primitive, index_type, count;
} NvnGapIndexedDraw;

typedef void (*NvnGapMethodVisitor)(u32 subchannel, u32 method, u32 value, void* context);

/* Visit only complete, well-formed cursor gaps. No callbacks run if validation fails. */
int nvn_command_gap_methods(const u32* words, u32 count, NvnGapMethodVisitor visit, void* context);

/* Recognize one complete indexed draw packet at the end of a command-buffer cursor gap. */
int nvn_command_gap_indexed_draw(const u32* words, u32 count, NvnGapIndexedDraw* out);
#endif
