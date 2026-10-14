/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
#ifndef SNX_NX_MODULE_ABI_H
#define SNX_NX_MODULE_ABI_H
#include "cpu.h"
#include "cpu/arm/arm.h"
/* the module descriptor: what snx_module() in the game module's module.c (written by src/nx/loader/module.c) hands
   the runtime's loader. abi stays the first field so a module from another version is recognized before anything
   else is read */
typedef struct SnxModInfo {
    const char* name;
    u64 base, size;
    u8 build_id[32];
    u32 text_off, text_size, ro_off, ro_size, data_off, data_size, bss_size;
    u32 is_nro;
} SnxModInfo;
typedef struct SnxModule {
    u32 abi;
    u32 nmods;
    const SnxModInfo* mods;
    u32 nentries;
    const u64* entry_addr;
    const GuestFn* entry_fn;
    GuestFn interp;
    u64 code_lo, code_hi;
    const char* title;
    u64 title_id;
    u32 main_stack, main_prio, main_core;
} SnxModule;

/* AArch32 (A32/Thumb) module descriptor, selected when the NPDM reports 32-bit
   (is64 = 0). Blocks are SnxArmBlock(void(SnxArmCPU*)). Syscalls share the A64
   HLE surface: the runtime supplies SnxArmSyscall, which zero-extends r0-r5
   into 64-bit arguments and calls the same handler A64 does (DRY). */
typedef void (*SnxArmSyscall)(SnxArmCPU* cpu, u32 imm);
typedef struct SnxArmModule {
    u32 abi;
    u32 nentries;
    const u64* entry_addr;
    const SnxArmBlock* entry_fn;
    SnxArmSyscall syscall;
    u32 code_lo, code_hi;
    const char* title;
    u64 title_id;
    u32 main_stack, main_prio, main_core;
} SnxArmModule;

#endif
