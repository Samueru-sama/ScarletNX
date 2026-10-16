/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The runtime JIT (jit_service.h). C90 in style; the target's C11 is for the LLVM-C headers and cpu.h's inline
   functions */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1 /* sched_getaffinity: the CPUs this process may run on */
#endif
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
typedef HANDLE JitThread;
#else
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
typedef pthread_mutex_t JitLock;
typedef pthread_cond_t JitCond;
typedef pthread_t JitThread;
#endif

/* What the service knows of a pc. QUEUED_WARM: in the warm queue only. QUEUED: in the misses' queue. IN_FLIGHT: a
   batch compiles from it. FAILED: the caller's interpreter runs it, every time, without another try. GONE: its image
   was removed (snx_jit_remove_image): the pc is new to the service if code turns up there again */
enum { QUEUED_WARM = 1, QUEUED, IN_FLIGHT, DONE, FAILED, GONE };
enum { LIVE, WARM, QUEUES }; /* the queues, by priority */

typedef struct {
    SnxU64 key; /* pc | 1 (a pc is 4-aligned), 0 for a free slot */
    GuestFn code;
    int state;
} Entry;

/* pcs to compile, first in first out: a ring that grows on its own */
typedef struct {
    SnxU64* pcs;
    size_t head, count, capacity; /* capacity: 0 or a power of two */
} Queue;

/* Main dispatch storage is immutable for the service lifetime. Dynamic regions, their tables and retirement
   state are protected by lock. A worker holds users on its one image from queue extraction through publication;
   retiring regions keep their address range reserved until that reference is released. */
typedef struct {
    SnxU64 lo, hi;
    GuestFn* slots;
    unsigned char* pending;
    SnxCodeImage image; /* an added region's */
    unsigned users;     /* under lock: batches compiling with its image */
    int retiring;
} Region;

struct SnxJitService {
    SnxJitConfig config;
    SnxCodeLoader* loader;
    GuestFn interpreter;
    Region regions[1 + SNX_JIT_IMAGES];
    /* the pcs: the misses and warm pcs queued, compiling or failed, and every leader of every compiled function, for
       a pc outside the dispatch table or one whose slot holds something else (a wrapper). An open-addressing table,
       at most half full, under lock */
    Entry* entries;
    size_t entry_count, entry_capacity; /* capacity: a power of two */
    Queue queues[QUEUES];               /* under lock */
    unsigned busy;                      /* batches compiling, under lock */
    int stop;                           /* under lock: the compile threads end */
    SnxJitStats stats;                  /* under lock */
    JitLock lock;                       /* everything above marked so */
    JitCond work;                       /* signalled when a pc is queued, broadcast at stop */
    JitCond idle;                       /* broadcast when nothing is queued or compiling */
    JitCond released;                   /* broadcast when a batch stops reading added images */
    JitLock load_lock;                  /* the loader, one load at a time (code_loader.h), and units */
    unsigned long units;                /* loads: the number in each unit's table name */
    JitThread threads[SNX_JIT_THREADS];
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
static void cond_signal(JitCond* c) {
#ifdef _WIN32
    WakeConditionVariable(c);
#else
    pthread_cond_signal(c);
#endif
}
static void cond_broadcast(JitCond* c) {
#ifdef _WIN32
    WakeAllConditionVariable(c);
#else
    pthread_cond_broadcast(c);
#endif
}

/* ---- the pcs and the queues, under lock ---- */

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

/* 0, or -1 when memory runs out */
static int push(Queue* q, SnxU64 pc) {
    SnxU64* grown;
    size_t capacity, i;
    if (q->count == q->capacity) {
        capacity = q->capacity ? q->capacity * 2 : 256;
        if (capacity > ((size_t)-1) / sizeof(*grown)) return -1;
        grown = (SnxU64*)malloc(capacity * sizeof(*grown));
        if (!grown) return -1;
        for (i = 0; i < q->count; ++i) grown[i] = q->pcs[(q->head + i) & (q->capacity - 1)];
        free(q->pcs);
        q->pcs = grown;
        q->head = 0;
        q->capacity = capacity;
    }
    q->pcs[(q->head + q->count) & (q->capacity - 1)] = pc;
    ++q->count;
    return 0;
}

static SnxU64 pop(Queue* q) {
    SnxU64 pc = q->pcs[q->head];
    q->head = (q->head + 1) & (q->capacity - 1);
    --q->count;
    return pc;
}

/* ---- image and table lookup (dynamic regions require lock) ---- */

/* pc in the text of the config's images */
static int in_text(const SnxJitService* s, SnxU64 pc) {
    const SnxCodeImage* m;
    size_t i;
    for (i = 0; !(pc & 3) && i < s->config.image_count; ++i) {
        m = &s->config.images[i];
        if (pc >= m->base + m->text_off && pc - (m->base + m->text_off) < m->text_size) return 1;
    }
    return 0;
}

/* the region whose table has a slot for pc, or NULL */
static Region* region_of(const SnxJitService* s, SnxU64 pc) {
    Region* r = (Region*)&s->regions[0];
    size_t i;
    if (pc & 3) return NULL;
    if (pc - r->lo < r->hi - r->lo) return r;
    for (i = 1; i <= SNX_JIT_IMAGES; ++i) {
        r = (Region*)&s->regions[i];
        if (!r->retiring && r->hi && pc >= r->lo && pc < r->hi) return r;
    }
    return NULL;
}

static GuestFn* slot(const Region* r, SnxU64 pc) { return &r->slots[(pc - r->lo) >> 2]; }

static int is_pending(const Region* r, SnxU64 pc) {
    size_t i = (size_t)((pc - r->lo) >> 2);
    return __atomic_load_n(&r->pending[i >> 3], __ATOMIC_RELAXED) >> (i & 7) & 1;
}

/* under lock, with the entry that says so in place: a thread that reads the bit without the lock gets NULL, which is
   right whatever the entry says by then (the code, once published, is in the slot) */
static void set_pending(SnxJitService* s, SnxU64 pc) {
    Region* r = region_of(s, pc);
    size_t i;
    if (!r) return;
    i = (size_t)((pc - r->lo) >> 2);
    __atomic_fetch_or(&r->pending[i >> 3], (unsigned char)(1u << (i & 7)), __ATOMIC_RELAXED);
}

/* ---- the compile ---- */

/* a batch's pcs by address, for discovery's question */
typedef struct {
    SnxJitService* service;
    const SnxU64* pcs;
    size_t count;
} Batch;

static int in_batch(const Batch* b, SnxU64 pc) {
    size_t lo = 0, hi = b->count, middle;
    while (lo < hi) {
        middle = lo + (hi - lo) / 2;
        if (b->pcs[middle] < pc)
            lo = middle + 1;
        else
            hi = middle;
    }
    return lo < b->count && b->pcs[lo] == pc;
}

/* snx_codegen_discover's question: code exists at address already (an AOT entry, an earlier batch's leader) or will
   without this batch (a pc queued, compiling in another batch, or failed) */
static int known(void* user, SnxU64 address) {
    const Batch* b = (const Batch*)user;
    SnxJitService* s = b->service;
    Region* r;
    Entry* e;
    int result;
    lock(&s->lock);
    r = region_of(s, address);
    if (r) {
        result = __atomic_load_n(slot(r, address), __ATOMIC_RELAXED) != NULL ||
                 (is_pending(r, address) && !in_batch(b, address));
        unlock(&s->lock);
        return result;
    }
    e = find(s, address);
    result = e && e->state != QUEUED_WARM && e->state != GONE && !(e->state == IN_FLIGHT && in_batch(b, address));
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

/* function i's code in a loaded unit's table */
static GuestFn unit_function(SnxU64 table, size_t i) {
    GuestFn code;
    memcpy(&code, (const unsigned char*)(size_t)table + i * sizeof(code), sizeof(code));
    return code;
}

/* Compiles the batch of pcs (count of them by address, each IN_FLIGHT), publishes the code and ends their entries:
   DONE with the code that enters at each, or FAILED. A batch of several that fails to emit or load is split, so one pc
   LLVM or the loader refuses fails alone */
/* Under lock: select a single owning image. Discovery must not inline code from another unloadable image. */
static const SnxCodeImage* image_of(SnxJitService* s, SnxU64 pc, Region** owner) {
    size_t i;
    Region* r;
    *owner = NULL;
    for (i = 0; i < s->config.image_count; ++i) {
        const SnxCodeImage* m = &s->config.images[i];
        if (!(pc & 3) && pc >= m->base + m->text_off && pc - (m->base + m->text_off) < m->text_size)
            return m;
    }
    r = region_of(s, pc);
    if (!r || r == &s->regions[0]) return NULL;
    *owner = r;
    return &r->image;
}

/* The worker owns the image reference even while a failed batch is retried one pc at a time. */
static void compile(SnxJitService* s, const SnxU64* pcs, size_t count, const SnxCodeImage* image) {
    SnxCodegen codegen;
    SnxJitReport report;
    SnxLinkInput input;
    unsigned char* object = NULL;
    size_t size = 0, i, j, used, failed = 0;
    char error[512], name[48];
    const char* names[1];
    SnxU64 table = 0, address;
    GuestFn code, expected;
    const SnxFunction* f;
    Region* r;
    Entry* e;
    Batch batch;
    int rc, discovered;
    memset(&report, 0, sizeof(report));
    report.pc = pcs[0];
    report.pcs = count;
    report.seconds = snx_seconds();
    error[0] = 0;
    batch.service = s;
    batch.pcs = pcs;
    batch.count = count;
    rc = snx_codegen_discover(&codegen, image, 1, pcs, count, known, &batch, s->config.budget);
    discovered = !rc;
    if (rc) snprintf(error, sizeof(error), "no guest code to compile at %llx", (unsigned long long)pcs[0]);
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
        /* the code is complete and visible to instruction fetch when this returns: its addresses may go to any
           thread from here (code_loader.h) */
        lock(&s->load_lock);
        used = snx_code_used(s->loader);
        rc = snx_code_load(s->loader, &input, 1, names, 1, &table, error, sizeof(error));
        report.bytes = snx_code_used(s->loader) - used;
        unlock(&s->load_lock);
    }
    free(object);
    if (rc && discovered && count > 1) {
        snx_codegen_free(&codegen);
        for (i = 0; i < count; ++i) compile(s, &pcs[i], 1, image);
        return;
    }
    /* publish: every leader's slot that is still empty, with release ordering, so a thread that loads the address
       (relaxed, cpu.h: x86-64 keeps loads in order) reaches the code and data written before it. A slot something
       else filled meanwhile (another batch, a wrapper) keeps what it holds */
    lock(&s->lock);
    for (i = 0; !rc && i < codegen.function_count; ++i) {
        f = &codegen.functions[i];
        code = unit_function(table, i);
        for (j = 0; j < f->count; ++j) {
            if (!f->leaders[j]) continue;
            address = snx_function_address(f, j);
            r = region_of(s, address); /* an added region being removed has none: its arrays stay until users is 0 */
            if (!r) continue;
            expected = NULL;
            __atomic_compare_exchange_n(slot(r, address), &expected, code, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
        }
    }
    for (i = 0; !rc && i < codegen.function_count; ++i) {
        f = &codegen.functions[i];
        code = unit_function(table, i);
        for (j = 0; j < f->count; ++j) {
            if (!f->leaders[j]) continue;
            address = snx_function_address(f, j);
            if (!in_text(s, address) && !region_of(s, address)) continue;
            /* a queued pc this batch compiled is done (its queue skips it); one another batch compiles is that
               batch's to end, as this batch's own pcs are below, and an earlier batch's keeps its code */
            e = add(s, snx_function_address(f, j), DONE);
            if (!e || e->state == IN_FLIGHT || e->state == FAILED || e->state == GONE || (e->state == DONE && e->code))
                continue;
            e->state = DONE;
            e->code = code;
        }
    }
    for (i = 0; i < count; ++i) {
        e = find(s, pcs[i]);
        f = rc ? NULL : snx_codegen_entry(&codegen, pcs[i]);
        code = f ? unit_function(table, (size_t)(f - codegen.functions)) : NULL;
        /* its image went while the batch compiled (snx_jit_remove_image marks it retiring under this lock): gone, not failed */
        if (!in_text(s, pcs[i]) && !region_of(s, pcs[i])) {
            if (e) {
                e->state = GONE;
                e->code = NULL;
            }
            continue;
        }
        if (e) {
            e->state = code ? DONE : FAILED;
            e->code = code;
        }
        if (code)
            ++s->stats.compiled;
        else
            ++failed;
    }
    report.seconds = snx_seconds() - report.seconds;
    ++s->stats.batches;
    s->stats.failures += (unsigned long)failed;
    if (!rc) {
        s->stats.functions += (unsigned long)report.functions;
        s->stats.instructions += (unsigned long)report.instructions;
        s->stats.bytes += report.bytes;
    }
    s->stats.seconds += report.seconds;
    unlock(&s->lock);
    if (failed && !error[0])
        snprintf(error, sizeof(error), "%lu of the batch's pcs are not where their code was found",
                 (unsigned long)failed);
    report.error = failed ? error : NULL;
    if (s->config.report) s->config.report(&report);
    snx_codegen_free(&codegen);
}

/* Under lock: up to max pcs of the first queue that has any into pcs, made IN_FLIGHT; 0 when both are empty. Skips
   the pcs a batch compiled while they waited */
static size_t take(SnxJitService* s, SnxU64* pcs, SnxCodeImage* image, Region** owner) {
    size_t n = 0, max;
    int q, state;
    SnxU64 pc;
    Entry* e;
    const SnxCodeImage *selected = NULL, *candidate;
    Region* region;
    *owner = NULL;
    for (q = LIVE; q < QUEUES && !n; ++q) {
        max = q == LIVE ? s->config.batch : (s->config.batch + 7) / 8;
        while (n < max && s->queues[q].count) {
            pc = s->queues[q].pcs[s->queues[q].head];
            candidate = image_of(s, pc, &region);
            if (n && candidate && candidate != selected) break;
            pop(&s->queues[q]);
            e = find(s, pc);
            state = q == LIVE ? QUEUED : QUEUED_WARM;
            /* compiled since as another batch's leader (whatever its slot holds: a wrapper isn't code for it), or moved
               to the misses' queue */
            if (!e || e->state != state) continue;
            if (!candidate) { e->state = GONE; continue; }
            if (!n) {
                selected = candidate;
                *image = *candidate;
                *owner = region;
                if (region) ++region->users;
            }
            e->state = IN_FLIGHT;
            set_pending(s, pc);
            pcs[n++] = pc;
        }
    }
    s->stats.queued = (unsigned long)(s->queues[LIVE].count + s->queues[WARM].count);
    return n;
}

static int compare_pc(const void* a, const void* b) {
    SnxU64 x = *(const SnxU64*)a, y = *(const SnxU64*)b;
    return x < y ? -1 : x > y;
}

/* A compile thread: batch after batch until the service stops. A batch that leaves pcs queued wakes another thread
   for them */
static void worker(SnxJitService* s) {
    SnxU64* pcs = (SnxU64*)malloc(s->config.batch * sizeof(*pcs));
    size_t n;
    SnxCodeImage image;
    Region* owner;
#if defined(__linux__)
    /* below the guest threads: when the host has no idle CPU, a compile waits, not the game */
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10);
#endif
    lock(&s->lock);
    while (pcs && !s->stop) {
        n = take(s, pcs, &image, &owner);
        if (!n) {
            if (!s->busy) cond_broadcast(&s->idle);
            cond_wait(&s->work, &s->lock);
            continue;
        }
        ++s->busy;
        if (s->queues[LIVE].count || s->queues[WARM].count) cond_signal(&s->work);
        unlock(&s->lock);
        qsort(pcs, n, sizeof(*pcs), compare_pc);
        compile(s, pcs, n, &image);
        lock(&s->lock);
        if (owner) {
            --owner->users;
            cond_broadcast(&s->released);
        }
        --s->busy;
    }
    if (!s->busy) cond_broadcast(&s->idle);
    unlock(&s->lock);
    free(pcs);
}

#ifdef _WIN32
static DWORD WINAPI worker_main(LPVOID s) {
    worker((SnxJitService*)s);
    return 0;
}
static int thread_start(JitThread* t, SnxJitService* s) {
    *t = CreateThread(NULL, 0, worker_main, s, 0, NULL);
    return *t ? 0 : -1;
}
static void thread_join(JitThread t) {
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
}
#else
static void* worker_main(void* s) {
    worker((SnxJitService*)s);
    return NULL;
}
static int thread_start(JitThread* t, SnxJitService* s) { return pthread_create(t, NULL, worker_main, s) ? -1 : 0; }
static void thread_join(JitThread t) { pthread_join(t, NULL); }
#endif

/* ---- the interface ---- */

GuestFn snx_jit_resolve(SnxJitService* s, SnxU64 pc) {
    GuestFn code = NULL;
    Region* r;
    Entry* e;
    /* Keep the immutable main table's frequent pending miss cheap. Dynamic tables require the lock through use. */
    r = &s->regions[0];
    if (!(pc & 3) && pc - r->lo < r->hi - r->lo && is_pending(r, pc) &&
        !__atomic_load_n(slot(r, pc), __ATOMIC_ACQUIRE)) return NULL;
    lock(&s->lock);
    r = region_of(s, pc);
    if (r && r != &s->regions[0]) code = __atomic_load_n(slot(r, pc), __ATOMIC_ACQUIRE);
    if (code || (!in_text(s, pc) && !(r && r != &s->regions[0]))) {
        unlock(&s->lock);
        return code;
    }
    e = find(s, pc);
    if (!e || e->state == QUEUED_WARM || e->state == GONE) {
        if (!e) e = add(s, pc, QUEUED);
        if (e && !push(&s->queues[LIVE], pc)) {
            e->state = QUEUED;
            set_pending(s, pc);
            s->stats.queued = (unsigned long)(s->queues[LIVE].count + s->queues[WARM].count);
            cond_signal(&s->work);
        } else if (e && e->state == QUEUED)
            e->state = FAILED; /* in no queue: memory ran out */
    } else if (e->state == DONE)
        code = e->code;
    unlock(&s->lock);
    return code;
}

size_t snx_jit_warm(SnxJitService* s, const SnxU64* pcs, size_t count) {
    size_t i, queued = 0;
    Region* r;
    Entry* e;
    lock(&s->lock);
    for (i = 0; i < count; ++i) {
        if (!in_text(s, pcs[i]) || find(s, pcs[i])) continue;
        r = region_of(s, pcs[i]);
        if (r && __atomic_load_n(slot(r, pcs[i]), __ATOMIC_RELAXED)) continue;
        e = add(s, pcs[i], QUEUED_WARM);
        if (!e) break;
        if (push(&s->queues[WARM], pcs[i])) {
            e->state = FAILED;
            break;
        }
        ++queued;
    }
    s->stats.queued = (unsigned long)(s->queues[LIVE].count + s->queues[WARM].count);
    cond_signal(&s->work);
    unlock(&s->lock);
    return queued;
}

void snx_jit_drain(SnxJitService* s) {
    lock(&s->lock);
    while (s->queues[LIVE].count || s->queues[WARM].count || s->busy) cond_wait(&s->idle, &s->lock);
    unlock(&s->lock);
}

GuestFn snx_jit_interpreter(SnxJitService* s) { return s->interpreter; }

int snx_jit_add_image(SnxJitService* s, const SnxCodeImage* image) {
    SnxU64 lo, hi;
    size_t i, n;
    GuestFn* slots;
    unsigned char* pending;
    Region* r = NULL;
    if (!image || !image->image || !image->text_size || (image->text_size & 3) ||
        image->text_off > image->image_size || image->text_size > image->image_size - image->text_off ||
        image->base > ~(SnxU64)0 - image->text_off) return -1;
    lo = image->base + image->text_off;
    if ((lo & 3) || image->text_size > ~(SnxU64)0 - lo ||
        image->text_size / 4 > ((size_t)-1) / sizeof(*slots)) return -1;
    hi = lo + image->text_size;
    n = (size_t)(image->text_size / 4);
    slots = (GuestFn*)calloc(n, sizeof(*slots));
    pending = (unsigned char*)calloc(n / 8 + 1, 1);
    if (!slots || !pending) {
        free(slots);
        free(pending);
        return -1;
    }
    lock(&s->lock);
    if (lo < s->regions[0].hi && s->regions[0].lo < hi) r = &s->regions[0]; /* over the main table: refused */
    for (i = 1; r != &s->regions[0] && i <= SNX_JIT_IMAGES; ++i)
        if (!s->regions[i].hi) {
            if (!r) r = &s->regions[i];
        } else if (lo < s->regions[i].hi && s->regions[i].lo < hi)
            r = &s->regions[0];
    if (!r || r == &s->regions[0]) {
        unlock(&s->lock);
        free(slots);
        free(pending);
        return -1;
    }
    r->slots = slots;
    r->pending = pending;
    r->image = *image;
    r->lo = lo;
    r->hi = hi;
    unlock(&s->lock);
    return 0;
}

void snx_jit_remove_image(SnxJitService* s, SnxU64 base) {
    Region* r = NULL;
    GuestFn* slots;
    unsigned char* pending;
    SnxU64 lo, hi, pc;
    size_t i;
    lock(&s->lock);
    for (i = 1; i <= SNX_JIT_IMAGES && !r; ++i)
        if (s->regions[i].hi && s->regions[i].image.base == base) r = &s->regions[i];
    if (!r) {
        unlock(&s->lock);
        return;
    }
    if (r->retiring) {
        /* A concurrent remover must also wait until the compiler releases these guest bytes. */
        while (r->retiring && r->image.base == base) cond_wait(&s->released, &s->lock);
        unlock(&s->lock);
        return;
    }
    lo = r->lo;
    hi = r->hi;
    r->retiring = 1; /* reserve its interval and storage, but prevent lookup and new compilation */
    while (r->users) cond_wait(&s->released, &s->lock);
    /* its pcs are new to the service again: queued ones are skipped when taken, compiled ones forgotten */
    for (i = 0; i < s->entry_capacity; ++i) {
        pc = s->entries[i].key & ~(SnxU64)1;
        if (!s->entries[i].key || pc < lo || pc >= hi) continue;
        s->entries[i].state = GONE;
        s->entries[i].code = NULL;
    }
    slots = r->slots;
    pending = r->pending;
    r->slots = NULL;
    r->pending = NULL;
    r->hi = 0;
    r->retiring = 0;
    memset(&r->image, 0, sizeof(r->image));
    cond_broadcast(&s->released);
    unlock(&s->lock);
    free(slots);
    free(pending);
}

/* the CPUs the process may run on: its affinity mask where the host has one (a game started under taskset gets the
   CPUs taskset gave it), else every online CPU */
static unsigned usable_cpus(void) {
#ifdef __linux__
    cpu_set_t set;
    if (!sched_getaffinity(0, sizeof(set), &set) && CPU_COUNT(&set) > 0) return (unsigned)CPU_COUNT(&set);
#endif
    return snx_cpu_count();
}
/* the compile threads when the config leaves it open: every CPU the process may use but two, from one up to
   SNX_JIT_THREADS. They run below the game's threads (worker lowers their priority), so a compile only gets a
   CPU the guest threads, the GPU thread and audio leave idle. Keeping six CPUs for the game left one compile thread on
   hosts with up to 7 CPUs and two on 8, which fell minutes behind Ender Magnolia's misses */
static unsigned default_threads(void) {
    unsigned cpus = usable_cpus();
    return cpus >= SNX_JIT_THREADS + 2 ? SNX_JIT_THREADS : cpus > 3 ? cpus - 2 : 1;
}

SnxJitService* snx_jit_service_new(const SnxJitConfig* config, char* error, size_t error_size) {
    SnxJitService* s;
    SnxLinkInput input;
    SnxU64 interpreter = 0;
    const char* interp_name = "snx_interp";
    size_t i, bits;
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
    if (!s->config.batch) s->config.batch = SNX_JIT_BATCH;
    if (!s->config.opt) s->config.opt = '2';
    if (!s->config.threads) s->config.threads = default_threads();
    if (s->config.threads > SNX_JIT_THREADS) s->config.threads = SNX_JIT_THREADS;
    bits = s->config.code_hi > s->config.code_lo ? (size_t)((s->config.code_hi - s->config.code_lo) >> 2) : 0;
    s->regions[0].lo = s->config.code_lo;
    s->regions[0].hi = s->config.code_hi > s->config.code_lo ? s->config.code_hi : s->config.code_lo;
    s->regions[0].slots = s->config.dispatch;
    s->regions[0].pending = (unsigned char*)calloc(bits / 8 + 1, 1);
    s->loader = s->regions[0].pending ? snx_code_loader_new(0, error, error_size) : NULL;
    if (!s->loader) {
        if (!s->regions[0].pending) snprintf(error, error_size, "out of memory");
        free(s->regions[0].pending);
        free(s);
        return NULL;
    }
    input.data = snx_rtlib_object;
    input.size = snx_rtlib_object_size;
    input.name = "rtlib";
    if (snx_code_load(s->loader, &input, 1, &interp_name, 1, &interpreter, error, error_size)) {
        snx_code_loader_free(s->loader);
        free(s->regions[0].pending);
        free(s);
        return NULL;
    }
    s->interpreter = (GuestFn)(size_t)interpreter;
    s->stats.bytes = snx_code_used(s->loader);
    lock_init(&s->lock);
    lock_init(&s->load_lock);
    cond_init(&s->work);
    cond_init(&s->idle);
    cond_init(&s->released);
    for (i = 0; i < s->config.threads; ++i)
        if (thread_start(&s->threads[i], s)) break;
    s->stats.threads = (unsigned)i;
    if (!i) {
        snprintf(error, error_size, "cannot start a compile thread");
        snx_jit_service_free(s);
        return NULL;
    }
    return s;
}

void snx_jit_service_free(SnxJitService* s) {
    unsigned i;
    int q;
    if (!s) return;
    lock(&s->lock);
    s->stop = 1;
    cond_broadcast(&s->work);
    unlock(&s->lock);
    for (i = 0; i < s->stats.threads; ++i) thread_join(s->threads[i]);
    snx_code_loader_free(s->loader);
    free(s->entries);
    for (q = 0; q < QUEUES; ++q) free(s->queues[q].pcs);
    for (i = 0; i <= SNX_JIT_IMAGES; ++i) {
        free(s->regions[i].pending);
        if (i) free(s->regions[i].slots);
    }
    cond_free(&s->work);
    cond_free(&s->idle);
    cond_free(&s->released);
    lock_free(&s->lock);
    lock_free(&s->load_lock);
    free(s);
}

void snx_jit_stats(SnxJitService* s, SnxJitStats* stats) {
    lock(&s->lock);
    *stats = s->stats;
    unlock(&s->lock);
}
