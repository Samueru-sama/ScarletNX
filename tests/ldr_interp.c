/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The same interpreter emitted into every NX code module. */
#include "cpu.h"
void snx_interp(CPU* c, u64 entry);
u64 snx_step(CPU* c, u32 word, u64 pc);
/* Unsupported encodings still decode operands before taking the interpreter's undefined-instruction path. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#include "ldr_interp.inc"
#pragma GCC diagnostic pop
GuestFn snx_interp_entry = snx_interp;
