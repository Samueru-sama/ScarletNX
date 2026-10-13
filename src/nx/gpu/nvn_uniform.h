/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_NVN_UNIFORM_H
#define SNX_NVN_UNIFORM_H
#include "gpu/nvn_draw.h"

/* Apply updates captured before a draw to its private uniform copy, oldest first.
   Returns zero for an invalid range or when the bounded update list cannot be staged. */
int nvn_uniform_updates_apply(const NvnUniformUpdate* head, u64 address, u32 size, u8* data);

#endif
