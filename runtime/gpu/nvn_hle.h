/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_NVN_HLE_H
#define SNX_NVN_HLE_H
#include "abi/cpu.h"

int nvn_hle_supported(const char* name);
/* Returns 1 if the call completed in HLE, 0 to execute its guest implementation. */
int nvn_hle_try(CPU* c, const char* name);
void nvn_hle_report(void);

#endif
