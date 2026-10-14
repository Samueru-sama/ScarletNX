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
#endif
