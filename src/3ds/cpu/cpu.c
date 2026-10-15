/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "ctr_cpu.h"
#include <string.h>

static uint32_t access_memory(void* context, uint32_t op, uint32_t va, uint32_t size, void* bytes) {
    SnxCtrAddressSpace* space = context;
    if (!size || size > 64 || !bytes) return 1;
    SnxCtrMemResult result;
    switch (op) {
        case SNX_ARM_ACCESS_READ: result = snx_ctr_read(space, va, bytes, size); break;
        case SNX_ARM_ACCESS_WRITE: result = snx_ctr_write(space, va, bytes, size); break;
        case SNX_ARM_ACCESS_FETCH: result = snx_ctr_fetch(space, va, bytes, size); break;
        case SNX_ARM_ACCESS_READ | SNX_ARM_ACCESS_MULTI: result = snx_ctr_read_ram(space, va, bytes, size); break;
        case SNX_ARM_ACCESS_WRITE | SNX_ARM_ACCESS_MULTI: result = snx_ctr_write_ram(space, va, bytes, size); break;
        default: return 1;
    }
    return result == SNX_CTR_MEM_OK ? 0 : 1;
}
bool snx_ctr_cpu_init(SnxCtrCpu* cpu, SnxCtrAddressSpace* space, uint32_t pc, uint32_t sp, bool thumb) {
    if (!cpu || !space || (pc & (thumb ? 1u : 3u)) || (sp & 7)) return false;
    memset(cpu, 0, sizeof *cpu);
    cpu->state.cpu.cpsr = 0x10u | (thumb ? SNX_ARM_T : 0);
    cpu->state.cpu.r[15] = pc;
    cpu->state.cpu.r[13] = sp;
    cpu->state.abi = SNX_ARM_CHECKED_ABI;
    cpu->state.context = space;
    cpu->state.access = access_memory;
    return true;
}
