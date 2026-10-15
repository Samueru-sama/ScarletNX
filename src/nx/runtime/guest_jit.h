/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What runs a pc the game module has no function for (RtAPI.miss, rtapi.c): with cpu_fallback=jit, code the runtime
   JIT compiles in the background (recompiler/llvm/jit_service.c), and the module's interpreter until it has */
#ifndef SNX_RUNTIME_GUEST_JIT_H
#define SNX_RUNTIME_GUEST_JIT_H
#include "cpu/aarch64/cpu.h"

/* once the game's executables are loaded, the dispatch table built and the hints loaded, before any guest thread
   runs. Queues the hints, the pcs earlier runs missed, behind every miss of this one. A JIT that can't start leaves
   the interpreter in charge, with a warning */
void jit_init(void);
/* the JIT's code for pc, or NULL: the interpreter runs pc, this time while the JIT compiles it, or every time
   (cpu_fallback=interp, a pc the JIT can't compile). Never waits for a compile */
GuestFn jit_resolve(u64 pc);
/* the totals, into the log, at exit */
void jit_report(void);

#endif
