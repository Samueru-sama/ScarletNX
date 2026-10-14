/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "arm_exec.h"
#include "cpu/arm/llvm.h"
#include "../abi/syscalls/syscalls.h"
#include "../abi/errno/errno.h"
#include <llvm-c/ExecutionEngine.h>
#include <llvm-c/Target.h>
#include <llvm-c/Core.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint64_t key; /* pc<<1 | thumb */
    SnxArmBlock fn;
} ArmBlock;
struct GnuArmExec {
    LLVMContextRef ctx;
    LLVMExecutionEngineRef jit;
    ArmBlock* blocks;
    size_t count, capacity;
};

/* Halfwords/words of guest code from pc to the end of its executable range.
   The values are decoded at compile time only; generated code never reads them. */
static size_t code_span(GnuProcess* p, uint32_t pc, unsigned unit) {
    for (size_t i = 0; i < p->elf.code_count; i++) {
        uint64_t lo = p->elf.code[i].address, hi = lo + p->elf.code[i].size;
        if (pc >= lo && pc < hi) return (size_t)((hi - pc) / unit);
    }
    return 0;
}

static SnxArmBlock arm_block(GnuArmExec* e, GnuProcess* p, uint32_t pc, int thumb) {
    uint64_t key = ((uint64_t)pc << 1) | (unsigned)thumb;
    for (size_t i = 0; i < e->count; i++)
        if (e->blocks[i].key == key) return e->blocks[i].fn;
    char name[40];
    snprintf(name, sizeof name, "a32_%08x_%d", pc, thumb);
    LLVMModuleRef module = LLVMModuleCreateWithNameInContext(name, e->ctx);
    size_t consumed = 0;
    if (thumb) {
        size_t n = code_span(p, pc, 2);
        if (n && !snx_thumb_llvm_block(module, name, (const uint16_t*)(p->memory.base + pc), n, pc, &consumed)) {
            LLVMDisposeModule(module);
            return NULL;
        }
    } else {
        size_t n = code_span(p, pc, 4);
        if (n && !snx_arm_llvm_block(module, name, (const uint32_t*)(p->memory.base + pc), n, pc, &consumed)) {
            LLVMDisposeModule(module);
            return NULL;
        }
    }
    LLVMAddModule(e->jit, module); /* MCJIT takes ownership */
    uint64_t address = LLVMGetFunctionAddress(e->jit, name);
    if (!address) return NULL;
    if (e->count == e->capacity) {
        e->capacity = e->capacity ? e->capacity * 2 : 64;
        e->blocks = realloc(e->blocks, e->capacity * sizeof *e->blocks);
    }
    e->blocks[e->count].key = key;
    SnxArmBlock fn = (SnxArmBlock)(uintptr_t)address;
    e->blocks[e->count++].fn = fn;
    return fn;
}

/* AArch32 (ARM EABI) syscall numbers differ from AArch64 asm-generic. Map the
   set the GNU ABI implements; anything else reports ENOSYS. */
static uint64_t arm_syscall_number(uint32_t nr) {
    switch (nr) {
        case 1: return GNU_NR_exit;
        case 248: return GNU_NR_exit_group;
        case 256: return GNU_NR_set_tid_address;
        case 240: return GNU_NR_futex;
        case 263: return GNU_NR_clock_gettime;
        case 174: return GNU_NR_rt_sigaction;
        case 175: return GNU_NR_rt_sigprocmask;
        case 122: return GNU_NR_uname;
        case 20: return GNU_NR_getpid;
        case 224: return GNU_NR_gettid;
        case 45: return GNU_NR_brk;
        case 91: return GNU_NR_munmap;
        case 192: return GNU_NR_mmap; /* mmap2: offset is in pages */
        case 125: return GNU_NR_mprotect;
        case 384: return GNU_NR_getrandom;
        default: return (uint64_t)-1;
    }
}
static void arm_syscall(GnuProcess* p) {
    uint64_t number = arm_syscall_number(p->arm.r[7]);
    if (number == (uint64_t)-1) {
        p->arm.r[0] = (uint32_t)-GNU_ENOSYS;
        return;
    }
    uint64_t arguments[6];
    for (unsigned i = 0; i < 6; i++) arguments[i] = p->arm.r[i];
    int64_t result = gnu_syscall(p, number, arguments);
    p->arm.r[0] = (uint32_t)result;
}

int gnu_arm_run(GnuProcess* p) {
    GnuArmExec e;
    memset(&e, 0, sizeof e);
    if (LLVMInitializeNativeTarget() || LLVMInitializeNativeAsmPrinter()) return 4;
    LLVMLinkInMCJIT();
    e.ctx = LLVMContextCreate();
    LLVMModuleRef initial = LLVMModuleCreateWithNameInContext("gnuarm", e.ctx);
    struct LLVMMCJITCompilerOptions options;
    LLVMInitializeMCJITCompilerOptions(&options, sizeof options);
    char* error = NULL;
    if (LLVMCreateMCJITCompilerForModule(&e.jit, initial, &options, sizeof options, &error)) {
        fprintf(stderr, "gnuabi: %s\n", error ? error : "cannot create ARM code cache");
        LLVMDisposeMessage(error);
        LLVMContextDispose(e.ctx);
        return 4;
    }
    if (!setjmp(p->exit_target)) {
        p->running = 1;
        while (!p->exited) {
            uint32_t pc = p->arm.r[15];
            int thumb = (p->arm.cpsr & SNX_ARM_T) != 0;
            SnxArmBlock block = arm_block(&e, p, pc, thumb);
            if (!block) {
                gnu_process_fault(p, 4, "Cannot compile an ARM guest block");
                break;
            }
            p->arm.exit = SNX_ARM_OK;
            block(&p->arm);
            switch (p->arm.exit) {
                case SNX_ARM_OK: break;
                case SNX_ARM_SVC: arm_syscall(p); break;
                case SNX_ARM_BREAKPOINT: gnu_process_fault(p, 5, "Guest executed BKPT"); break;
                case SNX_ARM_UNDEFINED: gnu_process_fault(p, 4, "Guest executed an undefined instruction"); break;
                case SNX_ARM_DATA_ABORT:
                    gnu_process_fault(p, 11, "Guest data access violated mapped permissions");
                    break;
                case SNX_ARM_ALIGNMENT: gnu_process_fault(p, 7, "Guest performed an unaligned access"); break;
                case SNX_ARM_ENDIAN_UNSUPPORTED:
                    gnu_process_fault(p, 4, "Big-endian guest state is unsupported");
                    break;
                default: gnu_process_fault(p, 4, "Guest entered an unsupported instruction state"); break;
            }
        }
        if (!p->exited) gnu_process_fault(p, 4, "Guest entry returned without exit/exit_group");
    }
    p->running = 0;
    LLVMDisposeExecutionEngine(e.jit);
    LLVMContextDispose(e.ctx);
    free(e.blocks);
    return p->exit_code;
}
