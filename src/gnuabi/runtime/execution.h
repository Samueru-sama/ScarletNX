/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_GNU_EXECUTION_H
#define SNX_GNU_EXECUTION_H
#include "gnuabi/process/process.h"
#include "common/cpu/execution/execution.h"
/* Single guest thread. Protected arena faults terminate the guest and allow
   compiler-module cleanup; guest signal-frame delivery is not implemented. */
int gnu_runtime_run(GnuProcess* process, SnxExecution* execution);
/* gnu_rtapi's miss: the running execution's interpreter */
void gnu_runtime_miss(CPU* cpu, uint64_t pc);
#endif
