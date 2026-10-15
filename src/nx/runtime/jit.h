/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What runs a pc the game module has no function for (RtAPI.miss, rtapi.c): with cpu_fallback=jit, code the runtime
   JIT compiles (recompiler/llvm/jit_service.c), else the module's interpreter */
#ifndef SNX_RUNTIME_JIT_H
#define SNX_RUNTIME_JIT_H
#include "cpu/aarch64/cpu.h"

/* once the game's executables are loaded and the dispatch table built, before any guest thread runs. A JIT that
   can't start leaves the interpreter in charge, with a warning */
void jit_init(void);
/* the JIT's code for pc, compiled now when it has none, or NULL: the interpreter runs pc (cpu_fallback=interp, a pc
   the JIT can't compile) */
GuestFn jit_resolve(CPU* c, u64 pc);
/* the totals, into the log, at exit */
void jit_report(void);

#endif
