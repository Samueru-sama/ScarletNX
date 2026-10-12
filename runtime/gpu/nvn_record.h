/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_NVN_RECORD_H
#define SNX_NVN_RECORD_H
#include "abi/cpu.h"

int nvn_record_wants(const char* name);
void nvn_record_before(const CPU* c, const char* name);
void nvn_record_after(const CPU* c, const char* name, u64 command_buffer);
void nvn_record_report(void);
#endif
