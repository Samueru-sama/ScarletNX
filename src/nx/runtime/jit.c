/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The runtime JIT's NX side (jit.h): the game's code ranges and the module's entries handed to the service, and a
   guest thread's core given up while it compiles */
#include "runtime/jit.h"
#include "runtime/loader.h"
#include "runtime/options.h"
#include "gpu/nvn_trace.h"
#include "hle/kernel/kernel.h"
#include "memory/mem.h"
#include "runtime/platform/log.h"
#include "jit_service.h"
#include <stdlib.h>
#include <string.h>

static SnxJitService* g_jit;
static SnxCodeImage* g_jit_images; /* the modules' text in guest memory, for as long as the service lives */

/* A compile takes milliseconds: a thread holding its core that long would stall the threads waiting for that core,
   which the console would have run meanwhile. The thread compiles off its core, as it waits in the kernel */
static void jit_leave(CPU* c) {
    (void)c;
    core_leave(kthread_cur());
}
static void jit_enter(CPU* c) {
    (void)c;
    core_enter(kthread_cur());
}

static void jit_log(const SnxJitReport* r) {
    if (r->error)
        WARN_LIMIT(32, "jit: %llx runs in the interpreter: %s\n", (unsigned long long)r->pc, r->error);
    else
        LOGV("jit: %llx compiled, %lu functions, %lu instructions, %lu KiB, %.1f ms\n", (unsigned long long)r->pc,
             (unsigned long)r->functions, (unsigned long)r->instructions, (unsigned long)(r->bytes >> 10),
             r->seconds * 1000.0);
}

void jit_init(void) {
    SnxJitConfig config;
    char error[512];
    u32 i;
    if (g_opt_cpu_fallback != CPU_FALLBACK_JIT) return;
    g_jit_images = (SnxCodeImage*)calloc(g_mod->nmods ? g_mod->nmods : 1, sizeof(*g_jit_images));
    if (!g_jit_images) {
        WARN("jit: out of memory, the interpreter runs the code the recompiler missed\n");
        return;
    }
    for (i = 0; i < g_mod->nmods; i++) {
        const SnxModInfo* m = &g_mod->mods[i];
        g_jit_images[i].image = GP(m->base);
        g_jit_images[i].image_size = (size_t)m->size;
        g_jit_images[i].base = m->base;
        g_jit_images[i].text_off = m->text_off;
        g_jit_images[i].text_size = m->text_size;
    }
    memset(&config, 0, sizeof(config));
    config.images = g_jit_images;
    config.image_count = g_mod->nmods;
    config.dispatch = g_disp;
    config.code_lo = g_code_lo;
    config.code_hi = g_code_hi;
    config.aot_addresses = g_mod->entry_addr;
    config.aot_functions = g_mod->entry_fn;
    config.aot_count = g_mod->nentries;
    /* with SNX_NVN_TRACE, wrappers sit in dispatch slots, which only calls through the table see */
    config.hook_direct_calls = nvn_trace_active();
    config.leave = jit_leave;
    config.enter = jit_enter;
    config.report = jit_log;
    g_jit = snx_jit_service_new(&config, error, sizeof(error));
    if (!g_jit) {
        WARN("jit: cannot start (%s), the interpreter runs the code the recompiler missed\n", error);
        free(g_jit_images);
        g_jit_images = NULL;
        return;
    }
    LOG("jit: guest code the recompiler missed is compiled when it first runs (cpu_fallback=jit)\n");
}

GuestFn jit_resolve(CPU* c, u64 pc) { return g_jit ? snx_jit_resolve(g_jit, c, pc) : NULL; }

void jit_report(void) {
    SnxJitStats s;
    if (!g_jit) return;
    snx_jit_stats(g_jit, &s);
    LOG("jit: %lu compiles (%lu failed), %lu functions, %lu instructions, %lu KiB of code, %.2f s compiling\n",
        s.compiles, s.failures, s.functions, s.instructions, (unsigned long)(s.bytes >> 10), s.seconds);
}
