/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_NVN_ALIAS_H
#define SNX_NVN_ALIAS_H
#include "abi/cpu.h"

enum { NVN_ALIAS_BUFFER = 1, NVN_ALIAS_TEXTURE = 2 };
void nvn_alias_enable(void);
void nvn_alias_register(int kind, u64 object, u64 pool, u64 generation, u64 offset, u64 size);
void nvn_alias_retire(int kind, u64 object);
void nvn_alias_retire_pool(u64 pool, u64 generation);
void nvn_alias_report(void);
#endif
