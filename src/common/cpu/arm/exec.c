/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "exec.h"
#include "llvm.h"
#include <llvm-c/ExecutionEngine.h>
#include <llvm-c/Target.h>
#include <llvm-c/Core.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint64_t key; /* pc<<1 | thumb */
    SnxArmBlock fn;
} CachedBlock;
struct SnxArmExec {
    LLVMContextRef ctx;
    LLVMExecutionEngineRef jit;
    CachedBlock* blocks;
    size_t count, capacity;
};

static size_t code_span(const SnxArmExecRange* ranges, size_t n, uint32_t pc, unsigned unit) {
    for (size_t i = 0; i < n; i++) {
        uint64_t lo = ranges[i].address, hi = lo + ranges[i].size;
        if (pc >= lo && pc < hi) return (size_t)((hi - pc) / unit);
    }
    return 0;
}

static SnxArmBlock arm_block(SnxArmExec* e, const SnxArmExecRange* ranges, size_t nranges, uint32_t pc, int thumb,
                             const unsigned char* base) {
    uint64_t key = ((uint64_t)pc << 1) | (unsigned)thumb;
    for (size_t i = 0; i < e->count; i++)
        if (e->blocks[i].key == key) return e->blocks[i].fn;
    char name[40];
    snprintf(name, sizeof name, "a32_%08x_%d", pc, thumb);
    LLVMModuleRef module = LLVMModuleCreateWithNameInContext(name, e->ctx);
    size_t consumed = 0, span = code_span(ranges, nranges, pc, thumb ? 2 : 4);
    LLVMValueRef fn = thumb ? snx_thumb_llvm_block(module, name, (const uint16_t*)(base + pc), span, pc, &consumed)
                            : snx_arm_llvm_block(module, name, (const uint32_t*)(base + pc), span, pc, &consumed);
    if (!fn) {
        LLVMDisposeModule(module);
        return NULL;
    }
    LLVMAddModule(e->jit, module); /* MCJIT takes ownership */
    uint64_t address = LLVMGetFunctionAddress(e->jit, name);
    if (!address) return NULL;
    if (e->count == e->capacity) {
        e->capacity = e->capacity ? e->capacity * 2 : 64;
        e->blocks = realloc(e->blocks, e->capacity * sizeof *e->blocks);
    }
    SnxArmBlock block = (SnxArmBlock)(uintptr_t)address;
    e->blocks[e->count].key = key;
    e->blocks[e->count++].fn = block;
    return block;
}

/* DRY: present A32 r0-r5 zero-extended to the 64-bit syscall surface, and r7
   as the number. The same HLE handler A64 uses then sees 64-bit values. */
static int run_syscall(SnxArmSyscallFn syscall, void* context, SnxArmCPU* cpu, uint32_t imm) {
    uint64_t arguments[6];
    for (unsigned i = 0; i < 6; i++) arguments[i] = cpu->r[i];
    return syscall(context, imm, cpu->r[7], arguments);
}

SnxArmExec* snx_arm_exec_new(char* error, size_t error_size) {
    if (error_size) error[0] = 0;
    if (LLVMInitializeNativeTarget() || LLVMInitializeNativeAsmPrinter()) {
        snprintf(error, error_size, "native LLVM target unavailable");
        return NULL;
    }
    LLVMLinkInMCJIT();
    SnxArmExec* e = calloc(1, sizeof *e);
    if (!e) {
        snprintf(error, error_size, "out of memory");
        return NULL;
    }
    e->ctx = LLVMContextCreate();
    LLVMModuleRef initial = LLVMModuleCreateWithNameInContext("a32exec", e->ctx);
    struct LLVMMCJITCompilerOptions options;
    LLVMInitializeMCJITCompilerOptions(&options, sizeof options);
    char* jit_error = NULL;
    if (LLVMCreateMCJITCompilerForModule(&e->jit, initial, &options, sizeof options, &jit_error)) {
        snprintf(error, error_size, "cannot create ARM code cache: %s", jit_error ? jit_error : "");
        LLVMDisposeMessage(jit_error);
        LLVMContextDispose(e->ctx);
        free(e);
        return NULL;
    }
    return e;
}

void snx_arm_exec_free(SnxArmExec* e) {
    if (!e) return;
    LLVMDisposeExecutionEngine(e->jit);
    LLVMContextDispose(e->ctx);
    free(e->blocks);
    free(e);
}

int snx_arm_exec_run(SnxArmExec* e, SnxArmCPU* cpu, const SnxArmExecRange* ranges, size_t range_count,
                     SnxArmSyscallFn syscall, void* context) {
    /* The code image base: blocks decode from it; generated code uses cpu->mem. */
    const unsigned char* base = range_count ? (const unsigned char*)cpu->mem : NULL;
    while (cpu->exit == SNX_ARM_OK) {
        uint32_t pc = cpu->r[15];
        int thumb = (cpu->cpsr & SNX_ARM_T) != 0;
        SnxArmBlock block = arm_block(e, ranges, range_count, pc, thumb, base);
        if (!block) {
            cpu->exit = SNX_ARM_BAD_ENTRY;
            break;
        }
        block(cpu);
        if (cpu->exit == SNX_ARM_SVC && syscall) {
            cpu->exit = SNX_ARM_OK;
            if (run_syscall(syscall, context, cpu, cpu->syndrome)) break;
        }
    }
    return (int)cpu->exit;
}
