/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What runs a pc the game module has no function for (RtAPI.miss, rtapi.c): with cpu_fallback=jit, code the runtime
   JIT compiles in the background (recompiler/llvm/jit_service.c), and the module's interpreter until it has */
#ifndef SNX_RUNTIME_GUEST_JIT_H
#define SNX_RUNTIME_GUEST_JIT_H
#include "common/cpu/aarch64/cpu.h"

/* once the game's executables are loaded, the dispatch table built and the hints loaded, before any guest thread
   runs. Queues the hints, the pcs earlier runs missed, behind every miss of this one. A JIT that can't start leaves
   the interpreter in charge, with a warning */
void jit_init(void);
/* the JIT's code for pc, or NULL: the interpreter runs pc, this time while the JIT compiles it, or every time
   (cpu_fallback=interp, a pc the JIT can't compile). Never waits for a compile */
GuestFn jit_resolve(u64 pc);
/* Code the game maps while it runs (an NRO, hle/services/ldr.c), text_size bytes of text at base out of size: the
   JIT compiles it as the module's, through a table of its own. 0, or -1 (no JIT, or as many such images as it keeps):
   the interpreter runs it */
int jit_code_added(u64 base, u64 text_size, u64 size);
/* before the memory at base is unmapped: waits for the compiles reading it, and forgets its code */
void jit_code_removed(u64 base);
/* the totals, into the log, at exit */
void jit_report(void);

#endif
