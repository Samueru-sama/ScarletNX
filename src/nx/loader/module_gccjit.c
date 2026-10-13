/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The libgccjit backend (--backend=gcc): reserved. A build would follow module_llvm.c, with libgccjit contexts in
   place of LLVM modules and gcc_jit_context_compile_to_file writing the shared library */
#include "module_llvm.h"

int snx_build_module_gccjit(SnxProgram* loaded, const SnxCodegenOptions* options, const SnxLlvmBuild* build) {
    (void)loaded;
    (void)options;
    (void)build;
    return snx_error("the gcc (libgccjit) backend is not implemented yet; use --backend=portable or --backend=llvm");
}
