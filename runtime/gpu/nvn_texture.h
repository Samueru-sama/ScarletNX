/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_NVN_TEXTURE_H
#define SNX_NVN_TEXTURE_H
#include "abi/cpu.h"

typedef struct {
    u64 texture, pool, offset;
    u32 flags, target, width, height, depth, levels, format, samples;
    int valid;
} NvnTextureCall;

int nvn_texture_wants(const char* name);
void nvn_texture_before(const CPU* c, const char* name, NvnTextureCall* call);
void nvn_texture_after(const CPU* c, const char* name, const NvnTextureCall* call);
void nvn_texture_report(void);
#endif
