/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The runtime JIT (recompiler/llvm/jit_service.c) driven as the runtime drives it: a few A64 functions in a guest
   memory buffer, a dispatch table with one "AOT" function (a host function standing in for the module's), and an
   RtAPI whose miss asks the service for code, as rtapi.c's does. Checks that a miss compiles the function and what it
   calls, publishes their leaders, calls the AOT function directly and other JIT code through the table, compiles a pc
   once when threads miss it together, serves a pc whose slot holds a wrapper from its own table, and refuses a pc
   outside the code */
#include "jit_service.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MEMORY  0x10000
#define CODE_LO 0x1000
#define CODE_HI 0x4000
#define THREADS 8

static int failures;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                                    \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

static unsigned char* memory;
static GuestFn dispatch[(CODE_HI - CODE_LO) / 4];
static SnxJitService* service;
static RtAPI rt;
static unsigned misses, leaves, enters, wrapped, unresolved;

static void put(u64 address, const u32* words, size_t count) { memcpy(memory + address, words, count * 4); }
static GuestFn* slot(u64 address) { return &dispatch[(address - CODE_LO) / 4]; }

/* the module's function at 0x2000: x0 += 100, then return */
static void aot_add100(CPU* c, u64 e) {
    (void)e;
    c->x[0] += 100;
    c->pc = c->x[30];
}
/* put in 0x2000's slot once the JIT has compiled its callers: their direct calls never see it */
static void aot_poisoned(CPU* c, u64 e) {
    (void)e;
    c->x[0] += 1000;
    c->pc = c->x[30];
}

/* rtapi.c's rt_miss, without the interpreter */
static void miss(CPU* c, u64 pc) {
    GuestFn f;
    __atomic_fetch_add(&misses, 1, __ATOMIC_RELAXED);
    f = snx_jit_resolve(service, c, pc);
    if (!f) {
        __atomic_fetch_add(&unresolved, 1, __ATOMIC_RELAXED);
        c->pc = 0;
        return;
    }
    f(c, pc);
}
static void poll(CPU* c) { __atomic_store_n(&c->poll, 0, __ATOMIC_RELAXED); }
static void undef(CPU* c, u64 pc, u32 w) {
    (void)c;
    fprintf(stderr, "undefined instruction %08x at %llx\n", w, (unsigned long long)pc);
    ++failures;
}
static void leave(CPU* c) {
    (void)c;
    __atomic_fetch_add(&leaves, 1, __ATOMIC_RELAXED);
}
static void enter(CPU* c) {
    (void)c;
    __atomic_fetch_add(&enters, 1, __ATOMIC_RELAXED);
}

/* the function at 0x3400 behind a wrapper, as gpu/nvn_trace.c puts one into an empty slot: its original is the miss */
static void wrapper(CPU* c, u64 e) {
    ++wrapped;
    rt.miss(c, e);
}

static void cpu_init(CPU* c, u64 stack) {
    memset(c, 0, sizeof(*c));
    c->mem = memory;
    c->disp = dispatch;
    c->code_lo = CODE_LO;
    c->code_hi = CODE_HI;
    c->rt = &rt;
    c->x[31] = stack;
    c->x[30] = 0xFFF0; /* where the outermost RET goes */
}

/* CALLDYN(pc), from the host: what a thread's loop does (kernel/thread.c) */
static void run(CPU* c, u64 pc) { snx_lookup(c, pc)(c, pc); }

typedef struct {
    pthread_barrier_t* barrier;
    CPU cpu;
    u64 pc;
} Racer;
static void* race(void* p) {
    Racer* r = (Racer*)p;
    pthread_barrier_wait(r->barrier);
    run(&r->cpu, r->pc);
    return NULL;
}
/* THREADS threads run racers[i].pc at once */
static void race_all(Racer* racers) {
    pthread_barrier_t barrier;
    pthread_t threads[THREADS];
    int i;
    pthread_barrier_init(&barrier, NULL, THREADS);
    for (i = 0; i < THREADS; ++i) {
        racers[i].barrier = &barrier;
        CHECK(!pthread_create(&threads[i], NULL, race, &racers[i]));
    }
    for (i = 0; i < THREADS; ++i) pthread_join(threads[i], NULL);
    pthread_barrier_destroy(&barrier);
}

int main(void) {
    /* 0x1000: x0 = x0 * 3 + 1, then the AOT function at 0x2000, then 0x3000, which the same compile finds */
    static const u32 f1[] = {0xD2800061, 0x9B017C00, 0x91000400, 0xA9BF7BFD,
                             0x940003FC, 0x940007FB, 0xA8C17BFD, 0xD65F03C0};
    static const u32 f3[] = {0x91001400, 0xD65F03C0}; /* 0x3000: x0 += 5 */
    /* 0x3800: x0 = 2 * f1(x0), calling 0x1000 through a register: CALLDYN to code of another unit */
    static const u32 f4[] = {0xA9BF7BFD, 0xD2820002, 0xD63F0040, 0x8B000000, 0xA8C17BFD, 0xD65F03C0};
    /* 0x2800: x0 += 100 in a loop, with a backward branch */
    static const u32 f5[] = {0xD2800C83, 0x91000400, 0xD1000463, 0xB5FFFFC3, 0xD65F03C0};
    static const u32 f6[] = {0x91002400, 0xD65F03C0}; /* 0x3400: x0 += 9 */
    /* 0x3C00 calls 0x3E00 (x0 += 3): threads miss both at once, so two compiles may each hold 0x3E00 */
    static const u32 f7[] = {0xA9BF7BFD, 0x9400007F, 0xA8C17BFD, 0xD65F03C0};
    static const u32 f8[] = {0x91000C00, 0xD65F03C0};
    static const u64 aot_addresses[] = {0x2000};
    static const GuestFn aot_functions[] = {aot_add100};
    SnxCodeImage image;
    SnxJitConfig config;
    SnxJitStats stats;
    Racer racers[THREADS];
    unsigned long before;
    CPU c;
    char error[512];
    int i;
    memory = (unsigned char*)calloc(1, MEMORY);
    if (!memory) return 1;
    put(0x1000, f1, 8);
    put(0x3000, f3, 2);
    put(0x3800, f4, 6);
    put(0x2800, f5, 5);
    put(0x3400, f6, 2);
    put(0x3C00, f7, 4);
    put(0x3E00, f8, 2);
    *slot(0x2000) = aot_add100; /* as main.c's build_dispatch fills the module's entries */
    rt.miss = miss;
    rt.poll = poll;
    rt.undef = undef;
    memset(&image, 0, sizeof(image));
    image.image = memory;
    image.image_size = MEMORY;
    image.base = 0;
    image.text_off = CODE_LO;
    image.text_size = CODE_HI - CODE_LO;
    memset(&config, 0, sizeof(config));
    config.images = &image;
    config.image_count = 1;
    config.dispatch = dispatch;
    config.code_lo = CODE_LO;
    config.code_hi = CODE_HI;
    config.aot_addresses = aot_addresses;
    config.aot_functions = aot_functions;
    config.aot_count = 1;
    config.leave = leave;
    config.enter = enter;
    service = snx_jit_service_new(&config, error, sizeof(error));
    if (!service) {
        fprintf(stderr, "jit: %s\n", error);
        return 1;
    }

    /* a miss at 0x1000 compiles it with 0x3000, and publishes both */
    cpu_init(&c, 0xF000);
    c.x[0] = 7;
    run(&c, 0x1000);
    CHECK(c.x[0] == 7 * 3 + 1 + 100 + 5 && c.pc == 0xFFF0 && c.x[31] == 0xF000);
    CHECK(misses == 1 && *slot(0x1000) && *slot(0x1014) && *slot(0x1018) && *slot(0x3000));
    snx_jit_stats(service, &stats);
    CHECK(stats.compiles == 1 && stats.functions == 2 && stats.instructions == 10 && !stats.failures);
    CHECK(leaves == 1 && enters == 1);
    /* the JIT's call to 0x2000 is direct: what its slot holds now doesn't matter */
    *slot(0x2000) = aot_poisoned;
    cpu_init(&c, 0xF000);
    c.x[0] = 1;
    run(&c, 0x1000);
    CHECK(c.x[0] == 1 * 3 + 1 + 100 + 5 && misses == 1);

    /* 0x3800 calls 0x1000 through the table */
    cpu_init(&c, 0xF000);
    c.x[0] = 1;
    run(&c, 0x3800);
    CHECK(c.x[0] == 2 * (1 * 3 + 1 + 100 + 5) && c.pc == 0xFFF0 && misses == 2);
    snx_jit_stats(service, &stats);
    CHECK(stats.compiles == 2 && stats.functions == 3);

    /* threads missing 0x2800 together: one compile, every thread runs the code */
    misses = 0;
    for (i = 0; i < THREADS; ++i) {
        cpu_init(&racers[i].cpu, 0x8800 + 0x800 * (u64)i);
        racers[i].cpu.x[0] = (u64)i;
        racers[i].pc = 0x2800;
    }
    race_all(racers);
    for (i = 0; i < THREADS; ++i) CHECK(racers[i].cpu.x[0] == (u64)i + 100 && racers[i].cpu.pc == 0xFFF0);
    snx_jit_stats(service, &stats);
    CHECK(stats.compiles == 3 && misses >= 1 && misses <= THREADS);
    CHECK(leaves == enters);

    /* half the threads miss a caller, half its callee: whichever publishes 0x3E00 first, every thread gets x0 + 3 */
    before = stats.compiles;
    for (i = 0; i < THREADS; ++i) {
        cpu_init(&racers[i].cpu, 0x8800 + 0x800 * (u64)i);
        racers[i].cpu.x[0] = (u64)i;
        racers[i].pc = i & 1 ? 0x3E00 : 0x3C00;
    }
    race_all(racers);
    for (i = 0; i < THREADS; ++i) CHECK(racers[i].cpu.x[0] == (u64)i + 3 && racers[i].cpu.pc == 0xFFF0);
    snx_jit_stats(service, &stats);
    CHECK(stats.compiles - before >= 1 && stats.compiles - before <= 2 && *slot(0x3C00) && *slot(0x3E00));
    before = stats.compiles;

    /* a wrapper in an empty slot: the miss compiles 0x3400 once, and the second call finds it in the service's table */
    *slot(0x3400) = wrapper;
    misses = 0;
    for (i = 0; i < 2; ++i) {
        cpu_init(&c, 0xF000);
        c.x[0] = 1;
        run(&c, 0x3400);
        CHECK(c.x[0] == 10 && c.pc == 0xFFF0);
    }
    snx_jit_stats(service, &stats);
    CHECK(wrapped == 2 && misses == 2 && stats.compiles == before + 1 && *slot(0x3400) == wrapper);

    /* refused: unaligned, outside the text */
    cpu_init(&c, 0xF000);
    CHECK(!snx_jit_resolve(service, &c, 0x1002) && !snx_jit_resolve(service, &c, 0x8000));
    CHECK(!unresolved);
    snx_jit_stats(service, &stats);
    CHECK(stats.compiles == before + 1 && !stats.failures && stats.bytes > 0);

    snx_jit_service_free(service);
    free(memory);
    if (failures) return 1;
    printf("jit service: %lu compiles, %lu KiB of code\n", stats.compiles, (unsigned long)(stats.bytes >> 10));
    return 0;
}
