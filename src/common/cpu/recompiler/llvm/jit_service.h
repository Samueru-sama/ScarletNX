/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The runtime JIT: guest code the AOT module lacks, compiled the first time a guest thread reaches it. snx_lookup
   (cpu.h) finds no function in CPU.disp and calls RtAPI.miss; the runtime's miss asks snx_jit_resolve for the code at
   pc. The service finds the function at pc and those it calls (snx_codegen_discover), emits them with the LLVM
   backend (snx_ir_emit, as the AOT build does), loads the object into its code arena (code_loader.c) and publishes
   every leader of every function into the dispatch table, so the next lookup finds it without a miss.

   One service per guest process, shared by the threads of its guests. A thread that misses a pc being compiled waits
   for it, so a pc is compiled once. Compiles of different pcs run in parallel (each emits its own LLVM module), only
   the loads into the arena are one at a time. A thread lets go of its core (config.leave and config.enter) while it
   waits or compiles, as it does in any other wait, so threads on its core keep running.

   The arena's first load is a copy of the runtime library (rtlib.c): JIT code calls its helpers directly, in reach of
   a 32-bit displacement. The library has no writable globals, so the copy shares nothing with the AOT module's.
   JIT code calls the AOT module's functions directly (config.aot_*), the functions of its own unit directly, and
   anything else through CPU.disp (CALLDYN), the code of other units included. Code is never freed: the arena lives as
   long as the service, which outlives every thread that runs its code.

   Not NX-specific: gnuabi's execution can create one too, with its code ranges and table */
#ifndef SNX_RECOMPILER_LLVM_JIT_SERVICE_H
#define SNX_RECOMPILER_LLVM_JIT_SERVICE_H
#include "codegen.h"
#include "cpu.h"

/* instructions a compile discovers beyond the function at pc (snx_codegen_discover's budget) */
#define SNX_JIT_BUDGET 20000

typedef struct SnxJitService SnxJitService;

/* one compile, for the log (config.report) */
typedef struct {
    SnxU64 pc;
    size_t functions, instructions, bytes; /* bytes: of the arena the load took */
    double seconds;
    const char* error; /* NULL when it compiled, else why the pc stays with the caller's fallback */
} SnxJitReport;

typedef struct {
    /* the guest code the JIT may compile: images[i].image is the guest memory at images[i].base, readable for as long
       as the service lives. A pc outside their text is refused */
    const SnxCodeImage* images;
    size_t image_count;
    /* CPU.disp: one slot per instruction of [code_lo, code_hi), which the service fills from NULL only */
    GuestFn* dispatch;
    SnxU64 code_lo, code_hi;
    /* the AOT module's entries by ascending address: JIT code calls these directly */
    const u64* aot_addresses;
    const GuestFn* aot_functions;
    size_t aot_count;
    /* every call through CPU.disp, as the AOT module's --hook-direct-calls (gpu/nvn_trace.c wraps slots) */
    int hook_direct_calls;
    size_t budget; /* 0: SNX_JIT_BUDGET */
    char opt;      /* the -O letter, 0: '2' */
    /* the calling thread gives its core up while it compiles or waits for another thread's compile, and takes it back
       (NULL: nothing to give up) */
    void (*leave)(CPU* c);
    void (*enter)(CPU* c);
    /* after each compile, outside every lock of the service (NULL: none) */
    void (*report)(const SnxJitReport* report);
} SnxJitConfig;

/* NULL with the reason in error: no code arena, a host the loader doesn't support (Windows for now), entries out of
   order. The config's arrays are used where they are */
SnxJitService* snx_jit_service_new(const SnxJitConfig* config, char* error, size_t error_size);
/* nothing may run the service's code any more */
void snx_jit_service_free(SnxJitService* service);
/* The guest function to call for pc (as GuestFn(c, pc)), compiled now if nothing has compiled it yet; c is the
   calling thread's CPU, only handed to config.leave and config.enter. NULL when pc can't be compiled (unaligned,
   outside the images, a compile that failed): the caller runs it its own way, every time */
GuestFn snx_jit_resolve(SnxJitService* service, CPU* c, SnxU64 pc);

typedef struct {
    unsigned long compiles, failures, functions, instructions;
    size_t bytes; /* of the arena, the runtime library's copy included */
    double seconds;
} SnxJitStats;
void snx_jit_stats(SnxJitService* service, SnxJitStats* stats);

#endif
