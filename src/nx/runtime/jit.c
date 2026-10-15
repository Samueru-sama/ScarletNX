/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The runtime JIT's NX side (jit.h): the game's code ranges and the module's entries handed to the service, and the
   hints of earlier runs queued for it */
#include "runtime/jit.h"
#include "runtime/loader.h"
#include "runtime/options.h"
#include "runtime/rtapi.h"
#include "gpu/nvn_trace.h"
#include "memory/mem.h"
#include "runtime/platform/log.h"
#include "jit_service.h"
#include <stdlib.h>
#include <string.h>

static SnxJitService* g_jit;
static SnxCodeImage* g_jit_images; /* the modules' text in guest memory, for as long as the service lives */

/* on a compile thread */
static void jit_log(const SnxJitReport* r) {
    if (r->error)
        WARN_LIMIT(32, "jit: %llx and %lu more pcs run in the interpreter: %s\n", (unsigned long long)r->pc,
                   (unsigned long)(r->pcs - 1), r->error);
    else
        LOGV("jit: %llx and %lu more pcs compiled, %lu functions, %lu instructions, %lu KiB, %.1f ms\n",
             (unsigned long long)r->pc, (unsigned long)(r->pcs - 1), (unsigned long)r->functions,
             (unsigned long)r->instructions, (unsigned long)(r->bytes >> 10), r->seconds * 1000.0);
}

void jit_init(void) {
    SnxJitConfig config;
    SnxJitStats stats;
    char error[512];
    u64* hints;
    size_t hint_count, warm = 0;
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
    config.threads = (unsigned)g_opt_jit_threads;
    config.report = jit_log;
    g_jit = snx_jit_service_new(&config, error, sizeof(error));
    if (!g_jit) {
        WARN("jit: cannot start (%s), the interpreter runs the code the recompiler missed\n", error);
        free(g_jit_images);
        g_jit_images = NULL;
        return;
    }
    /* warm start: what earlier runs missed is compiled before the game reaches it, if the JIT has time before then */
    hints = hints_copy(&hint_count);
    if (hints) warm = snx_jit_warm(g_jit, hints, hint_count);
    free(hints);
    snx_jit_stats(g_jit, &stats);
    LOG("jit: guest code the recompiler missed is compiled in the background on %u threads, the interpreter runs it "
        "meanwhile (cpu_fallback=jit). %lu pcs of earlier runs queued\n",
        stats.threads, (unsigned long)warm);
}

GuestFn jit_resolve(u64 pc) { return g_jit ? snx_jit_resolve(g_jit, pc) : NULL; }

void jit_report(void) {
    SnxJitStats s;
    if (!g_jit) return;
    snx_jit_stats(g_jit, &s);
    LOG("jit: %lu batches compiled %lu pcs (%lu failed, %lu still queued), %lu functions, %lu instructions, %lu KiB of "
        "code, %.2f s compiling\n",
        s.batches, s.compiled, s.failures, s.queued, s.functions, s.instructions, (unsigned long)(s.bytes >> 10),
        s.seconds);
}
