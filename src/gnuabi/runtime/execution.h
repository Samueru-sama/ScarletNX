/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_GNU_EXECUTION_H
#define SNX_GNU_EXECUTION_H
#include "../process/process.h"
#include "cpu/execution/execution.h"
/* Single guest thread. Protected arena faults terminate the guest and allow
   compiler-module cleanup; guest signal-frame delivery is not implemented. */
int gnu_runtime_run(GnuProcess* process, SnxExecution* execution);
#endif
