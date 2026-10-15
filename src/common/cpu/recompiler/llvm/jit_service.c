/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The runtime JIT (jit_service.h). C90 in style; the target's C11 is for the LLVM-C headers and cpu.h's inline
   functions */
#include "jit_service.h"
#include "code_loader.h"
#include "ir.h"
#include "rtlib_object.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
typedef SRWLOCK JitLock;
typedef CONDITION_VARIABLE JitCond;
#else
#include <pthread.h>
typedef pthread_mutex_t JitLock;
typedef pthread_cond_t JitCond;
#endif

/* what the service knows of a pc. IN_FLIGHT: a thread compiles from it, the others wait. FAILED: the caller's
   fallback runs it, every time, without another try */
enum { IN_FLIGHT = 1, DONE, FAILED };

typedef struct {
    SnxU64 key; /* pc | 1 (a pc is 4-aligned), 0 for a free slot */
    GuestFn code;
    int state;
} Entry;

struct SnxJitService {
    SnxJitConfig config;
    SnxCodeLoader* loader;
    /* the pcs: the misses compiling or failed, and every leader of every compiled function, for a pc outside the
       dispatch table or one whose slot holds something else (a wrapper). An open-addressing table, at most half full,
       under lock */
    Entry* entries;
    size_t entry_count, entry_capacity; /* capacity: a power of two */
    SnxJitStats stats;                  /* under lock */
    JitLock lock;                       /* entries and stats */
    JitCond done;                       /* broadcast when a compile ends */
    JitLock load_lock;                  /* the loader, one load at a time (code_loader.h), and units */
    unsigned long units;                /* loads: the number in each unit's table name */
};

static void lock_init(JitLock* l) {
#ifdef _WIN32
    InitializeSRWLock(l);
#else
    pthread_mutex_init(l, NULL);
#endif
}
static void lock_free(JitLock* l) {
#ifdef _WIN32
    (void)l;
#else
    pthread_mutex_destroy(l);
#endif
}
static void lock(JitLock* l) {
#ifdef _WIN32
    AcquireSRWLockExclusive(l);
#else
    pthread_mutex_lock(l);
#endif
}
static void unlock(JitLock* l) {
#ifdef _WIN32
    ReleaseSRWLockExclusive(l);
#else
    pthread_mutex_unlock(l);
#endif
}
static void cond_init(JitCond* c) {
#ifdef _WIN32
    InitializeConditionVariable(c);
#else
    pthread_cond_init(c, NULL);
#endif
}
static void cond_free(JitCond* c) {
#ifdef _WIN32
    (void)c;
#else
    pthread_cond_destroy(c);
#endif
}
static void cond_wait(JitCond* c, JitLock* l) {
#ifdef _WIN32
    SleepConditionVariableSRW(c, l, INFINITE, 0);
#else
    pthread_cond_wait(c, l);
#endif
}
static void cond_broadcast(JitCond* c) {
#ifdef _WIN32
    WakeAllConditionVariable(c);
#else
    pthread_cond_broadcast(c);
#endif
}

/* ---- the pcs, under lock ---- */

static size_t entry_hash(SnxU64 key, size_t capacity) {
    return (size_t)(((key >> 2) * (SnxU64)0x9E3779B97F4A7C15ULL) >> 32) & (capacity - 1);
}

static Entry* find(const SnxJitService* s, SnxU64 pc) {
    SnxU64 key = pc | 1;
    size_t i;
    if (!s->entry_capacity) return NULL;
    for (i = entry_hash(key, s->entry_capacity); s->entries[i].key; i = (i + 1) & (s->entry_capacity - 1))
        if (s->entries[i].key == key) return &s->entries[i];
    return NULL;
}

/* pc's entry, added with state when it has none. NULL when memory runs out */
static Entry* add(SnxJitService* s, SnxU64 pc, int state) {
    SnxU64 key = pc | 1;
    Entry* e = find(s, pc);
    size_t i;
    if (e) return e;
    if ((s->entry_count + 1) * 2 > s->entry_capacity) {
        size_t capacity = s->entry_capacity ? s->entry_capacity * 2 : 1024, j;
        Entry* grown;
        if (capacity > ((size_t)-1) / sizeof(*grown)) return NULL;
        grown = (Entry*)calloc(capacity, sizeof(*grown));
        if (!grown) return NULL;
        for (j = 0; j < s->entry_capacity; ++j)
            if (s->entries[j].key) {
                i = entry_hash(s->entries[j].key, capacity);
                while (grown[i].key) i = (i + 1) & (capacity - 1);
                grown[i] = s->entries[j];
            }
        free(s->entries);
        s->entries = grown;
        s->entry_capacity = capacity;
    }
    for (i = entry_hash(key, s->entry_capacity); s->entries[i].key; i = (i + 1) & (s->entry_capacity - 1)) continue;
    e = &s->entries[i];
    e->key = key;
    e->code = NULL;
    e->state = state;
    ++s->entry_count;
    return e;
}

/* ---- the compile ---- */

static int in_table(const SnxJitService* s, SnxU64 pc) {
    return !(pc & 3) && pc - s->config.code_lo < s->config.code_hi - s->config.code_lo;
}

static GuestFn* slot(const SnxJitService* s, SnxU64 pc) { return &s->config.dispatch[(pc - s->config.code_lo) >> 2]; }

/* snx_codegen_discover's question: code exists at address already, an AOT entry or an earlier compile's leader */
static int known(void* user, SnxU64 address) {
    SnxJitService* s = (SnxJitService*)user;
    Entry* e;
    int result;
    if (in_table(s, address)) return __atomic_load_n(slot(s, address), __ATOMIC_RELAXED) != NULL;
    lock(&s->lock);
    e = find(s, address);
    result = e && e->state == DONE;
    unlock(&s->lock);
    return result;
}

/* the AOT function to call for target, as a host address (SnxIr.direct), 0 when the AOT module has no entry there */
static SnxU64 direct(void* user, SnxU64 target) {
    const SnxJitService* s = (const SnxJitService*)user;
    size_t lo = 0, hi = s->config.aot_count, middle;
    while (lo < hi) {
        middle = lo + (hi - lo) / 2;
        if (s->config.aot_addresses[middle] < target)
            lo = middle + 1;
        else
            hi = middle;
    }
    if (lo == s->config.aot_count || s->config.aot_addresses[lo] != target || !s->config.aot_functions[lo]) return 0;
    return (SnxU64)(size_t)s->config.aot_functions[lo];
}

/* The unit's object: every function of codegen, internal to it, and table_name, an array of their addresses in
   codegen's order, its only global. Internal functions give two units the same F_<address> without a clash in the
   loader's table, where every global stays */
static int emit_unit(SnxJitService* s, SnxCodegen* codegen, const char* table_name, unsigned char** object,
                     size_t* size, char* error, size_t error_size) {
    SnxIr* ir = snx_ir_new("jit", s->config.opt, error, error_size);
    LLVMValueRef *functions, table;
    LLVMTypeRef type;
    size_t i;
    int rc;
    if (!ir) return -1;
    functions = (LLVMValueRef*)malloc(codegen->function_count * sizeof(*functions));
    if (!functions) {
        snx_ir_free(ir);
        snprintf(error, error_size, "out of memory");
        return -1;
    }
    ir->direct = direct;
    ir->direct_user = s;
    codegen->hook_direct_calls = s->config.hook_direct_calls;
    for (i = 0; i < codegen->function_count; ++i) snx_ir_emit(ir, codegen, &codegen->functions[i]);
    for (i = 0; i < codegen->function_count; ++i) {
        functions[i] = snx_ir_guest_function(ir, codegen->functions[i].start);
        LLVMSetLinkage(functions[i], LLVMInternalLinkage);
        LLVMSetVisibility(functions[i], LLVMDefaultVisibility); /* a local symbol has no visibility */
    }
    type = LLVMArrayType(ir->t_ptr, (unsigned)codegen->function_count);
    table = LLVMAddGlobal(ir->mod, type, table_name);
    LLVMSetInitializer(table, LLVMConstArray(ir->t_ptr, functions, (unsigned)codegen->function_count));
    LLVMSetGlobalConstant(table, 1);
    LLVMSetVisibility(table, LLVMHiddenVisibility);
    free(functions);
    rc = snx_ir_object(ir, s->config.opt, object, size, error, error_size);
    snx_ir_free(ir);
    return rc;
}

/* Compiles from pc, whose entry is IN_FLIGHT, publishes the code and ends the entry: the function at pc, or NULL */
static GuestFn compile(SnxJitService* s, SnxU64 pc) {
    SnxCodegen codegen;
    SnxJitReport report;
    SnxLinkInput input;
    unsigned char* object = NULL;
    size_t size = 0, i, j, used;
    char error[512], name[48];
    const char* names[1];
    SnxU64 table = 0, address;
    GuestFn result = NULL, code, expected;
    const SnxFunction* f;
    Entry* e;
    int rc;
    memset(&report, 0, sizeof(report));
    report.pc = pc;
    report.seconds = snx_seconds();
    error[0] = 0;
    rc = snx_codegen_discover(&codegen, s->config.images, s->config.image_count, pc, known, s, s->config.budget);
    if (rc) snprintf(error, sizeof(error), "no guest code to compile at %llx", (unsigned long long)pc);
    for (i = 0; !rc && i < codegen.function_count; ++i) report.instructions += codegen.functions[i].count;
    report.functions = codegen.function_count;
    if (!rc) {
        lock(&s->load_lock);
        sprintf(name, "snx_jit_unit_%lu", ++s->units);
        unlock(&s->load_lock);
        rc = emit_unit(s, &codegen, name, &object, &size, error, sizeof(error));
    }
    if (!rc) {
        input.data = object;
        input.size = size;
        input.name = name;
        names[0] = name;
        /* the code is executable and its addresses final when this returns: from here they may go to any thread */
        lock(&s->load_lock);
        used = snx_code_used(s->loader);
        rc = snx_code_load(s->loader, &input, 1, names, 1, &table, error, sizeof(error));
        report.bytes = snx_code_used(s->loader) - used;
        unlock(&s->load_lock);
    }
    free(object);
    /* publish: every leader's slot that is still empty, with release ordering, so a thread that loads the address
       (relaxed, cpu.h) reaches the code and data written before it. A slot something else filled meanwhile (another
       compile, a wrapper) keeps what it holds */
    for (i = 0; !rc && i < codegen.function_count; ++i) {
        f = &codegen.functions[i];
        memcpy(&code, (const unsigned char*)(size_t)table + i * sizeof(code), sizeof(code));
        if (f->start == pc) result = code;
        for (j = 0; j < f->count; ++j) {
            if (!f->leaders[j]) continue;
            address = snx_function_address(f, j);
            if (!in_table(s, address)) continue;
            expected = NULL;
            __atomic_compare_exchange_n(slot(s, address), &expected, code, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
        }
    }
    lock(&s->lock);
    for (i = 0; !rc && i < codegen.function_count; ++i) {
        f = &codegen.functions[i];
        memcpy(&code, (const unsigned char*)(size_t)table + i * sizeof(code), sizeof(code));
        for (j = 0; j < f->count; ++j) {
            if (!f->leaders[j]) continue;
            /* an entry another miss holds (IN_FLIGHT, FAILED) is that miss's to end, and an earlier compile's keeps
               its code */
            e = add(s, snx_function_address(f, j), DONE);
            if (e && e->state == DONE && !e->code) e->code = code;
        }
    }
    e = find(s, pc);
    if (e) {
        e->state = result ? DONE : FAILED;
        e->code = result;
    }
    report.seconds = snx_seconds() - report.seconds;
    if (result) {
        ++s->stats.compiles;
        s->stats.functions += (unsigned long)report.functions;
        s->stats.instructions += (unsigned long)report.instructions;
        s->stats.bytes += report.bytes;
    } else
        ++s->stats.failures;
    s->stats.seconds += report.seconds;
    cond_broadcast(&s->done);
    unlock(&s->lock);
    if (!result && !error[0])
        snprintf(error, sizeof(error), "the code at %llx is not where it was compiled", (unsigned long long)pc);
    report.error = result ? NULL : error;
    if (s->config.report) s->config.report(&report);
    snx_codegen_free(&codegen);
    return result;
}

GuestFn snx_jit_resolve(SnxJitService* s, CPU* c, SnxU64 pc) {
    const SnxCodeImage* m;
    GuestFn code = NULL;
    Entry* e;
    size_t i;
    int inside = 0, left = 0, mine = 0;
    /* step 3 of the JIT: NRO text the game loads joins the images here, published through the entries only */
    for (i = 0; !(pc & 3) && !inside && i < s->config.image_count; ++i) {
        m = &s->config.images[i];
        inside = pc >= m->base + m->text_off && pc - (m->base + m->text_off) < m->text_size;
    }
    if (!inside) return NULL;
    lock(&s->lock);
    for (;;) {
        e = find(s, pc);
        if (!e) {
            mine = add(s, pc, IN_FLIGHT) != NULL;
            break;
        }
        if (e->state != IN_FLIGHT) {
            code = e->state == DONE ? e->code : NULL;
            break;
        }
        if (!left && s->config.leave) { /* not under lock: giving the core up takes the kernel's */
            unlock(&s->lock);
            s->config.leave(c);
            left = 1;
            lock(&s->lock);
            continue;
        }
        cond_wait(&s->done, &s->lock);
    }
    unlock(&s->lock);
    if (mine) {
        if (!left && s->config.leave) s->config.leave(c);
        left = 1;
        code = compile(s, pc);
    }
    /* taking the core back may wait for it, so never under lock: its holder may be missing too */
    if (left && s->config.enter) s->config.enter(c);
    return code;
}

SnxJitService* snx_jit_service_new(const SnxJitConfig* config, char* error, size_t error_size) {
    SnxJitService* s;
    SnxLinkInput input;
    size_t i;
    for (i = 1; i < config->aot_count; ++i)
        if (config->aot_addresses[i - 1] >= config->aot_addresses[i]) {
            snprintf(error, error_size, "the AOT entries are not in ascending order");
            return NULL;
        }
    if (snx_ir_init(error, error_size)) return NULL;
    s = (SnxJitService*)calloc(1, sizeof(*s));
    if (!s) {
        snprintf(error, error_size, "out of memory");
        return NULL;
    }
    s->config = *config;
    if (!s->config.budget) s->config.budget = SNX_JIT_BUDGET;
    if (!s->config.opt) s->config.opt = '2';
    s->loader = snx_code_loader_new(0, error, error_size);
    if (!s->loader) {
        free(s);
        return NULL;
    }
    input.data = snx_rtlib_object;
    input.size = snx_rtlib_object_size;
    input.name = "rtlib";
    if (snx_code_load(s->loader, &input, 1, NULL, 0, NULL, error, error_size)) {
        snx_code_loader_free(s->loader);
        free(s);
        return NULL;
    }
    s->stats.bytes = snx_code_used(s->loader);
    lock_init(&s->lock);
    lock_init(&s->load_lock);
    cond_init(&s->done);
    return s;
}

void snx_jit_service_free(SnxJitService* s) {
    if (!s) return;
    snx_code_loader_free(s->loader);
    free(s->entries);
    cond_free(&s->done);
    lock_free(&s->lock);
    lock_free(&s->load_lock);
    free(s);
}

void snx_jit_stats(SnxJitService* s, SnxJitStats* stats) {
    lock(&s->lock);
    *stats = s->stats;
    unlock(&s->lock);
}
