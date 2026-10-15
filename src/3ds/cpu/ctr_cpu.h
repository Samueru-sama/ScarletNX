/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CTR_CPU_H
#define SNX_CTR_CPU_H
#include "cpu/arm/checked.h"
#include "memory/ctr_memory.h"

/* Opt-in checked A32 state, no NX/A64 CPU casts and no LLVM/JIT dependency.
   This adapter does NOT discover/load native blocks or implement an OS. */
typedef struct { SnxArmCheckedCPU state; } SnxCtrCpu;
/* Fresh USR state. Caller owns process mappings and executable entry validation.
   Space must outlive the CPU; all accesses/mapping changes are serialized. */
bool snx_ctr_cpu_init(SnxCtrCpu* cpu, SnxCtrAddressSpace* space, uint32_t pc, uint32_t sp, bool thumb);
#endif
