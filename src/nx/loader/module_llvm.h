/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The game module from the LLVM backend: what module.c and toolchain.c produce through C and Clang, written directly
   (LLVM IR per chunk, compiled in parallel, linked with the runtime library by recompiler/llvm/link.c) */
#ifndef SNX_NX_MODULE_LLVM_H
#define SNX_NX_MODULE_LLVM_H

#include "module.h"

typedef struct {
    const char* output; /* <Game>_code.so or .dll, replaced only by a complete module */
    char opt;           /* -O letter */
    unsigned jobs;
    int quiet;
    const char* keep_ir; /* when set, each unit's IR is saved there as .ll */
} SnxLlvmBuild;

/* 0 on success. Available when the recompiler was built with SNX_WITH_LLVM */
int snx_build_module_llvm(SnxProgram* loaded, const SnxCodegenOptions* options, const SnxLlvmBuild* build);
/* The libgccjit backend's entry point: reserved, it reports that it isn't implemented */
int snx_build_module_gccjit(SnxProgram* loaded, const SnxCodegenOptions* options, const SnxLlvmBuild* build);

#endif
