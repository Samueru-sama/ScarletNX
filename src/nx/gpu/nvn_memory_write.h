/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_NVN_MEMORY_WRITE_H
#define SNX_NVN_MEMORY_WRITE_H
#include "cpu/aarch64/cpu.h"

/* A bounded guest-GPU memory write whose effect on semantic images is resolved on the GPU thread. */
typedef struct {
    u64 sequence, prior_unmodeled_write;
    u64 address;
    u32 size;
    u8 kind; /* NVN_GAP_WRITE_* from the decoded command-memory packet */
} NvnMemoryWriteCommand;
#endif
