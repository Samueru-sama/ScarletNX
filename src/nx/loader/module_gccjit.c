/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The game module through libgccjit (--backend=gcc), for DEC Alpha EV67 hosts, where LLVM has no backend; elsewhere
   --backend=llvm is the one to use. module_llvm.c's structure: chunks of whole functions, each a context compiled
   to an object file, the chunks in parallel as module_llvm.c compiles its units, then the module descriptor in a
   last context that GCC's driver links into the shared library with the chunks and the runtime library (rtlib.c).
   The parallel workers are processes, not threads: libgccjit compiles one context at a time per process (GCC's
   "big mutex" around the compile, gcc/jit/jit-playback.cc), so threads would all share one core. fork() hands the
   workers the codegen and the queue copy-on-write; on Windows, which has no fork, they are threads (working, but
   serialized inside libgccjit) and the LLVM backend is the one to use anyway. */
#include "module_llvm.h"

#ifndef SNX_WITH_GCCJIT
int snx_build_module_gccjit(SnxProgram* loaded, const SnxCodegenOptions* options, const SnxLlvmBuild* build) {
    (void)loaded;
    (void)options;
    (void)build;
    return snx_error("this recompiler was built without libgccjit (SNX_GCCJIT); use --backend=llvm");
}
#else
#include "codegen.h"
#include "codegen_view.h"
#include "jit.h"
#include "module_abi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

/* the runtime library, compiled when the recompiler is built (rtlib.c, snx-rtlib-gen embed) */
extern const unsigned char snx_rtlib_object[];
extern const unsigned long snx_rtlib_object_size;

/* One chunk of functions compiled to one object file, and the queue the workers take their chunks from. The queue
   and the units live in memory the workers share (module_llvm.c's share an address space; forked ones need
   MAP_SHARED to see the work handover, the results and the progress count), so the object path is a buffer here,
   not a string each worker would own. */
typedef struct {
    size_t first, last; /* functions */
    char object[1024];
    unsigned long fallbacks;
} Unit;

typedef struct {
    SnxCodegen* codegen;
    const SnxLlvmBuild* build;
    const char* dir; /* the temporary directory the object files are written to */
    Unit* units;
    size_t count, next, done;
    int failed;
    char error[1024];
#ifdef _WIN32
    CRITICAL_SECTION lock;
#else
    pthread_mutex_t lock;
#endif
} Queue;

static void* shared_alloc(size_t size) {
#ifdef _WIN32
    return snx_alloc(size);
#else
    void* p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;
#endif
}
static void shared_free(void* p, size_t size) {
#ifdef _WIN32
    (void)size;
    free(p);
#else
    if (p) munmap(p, size);
#endif
}

static void lock(Queue* q) {
#ifdef _WIN32
    EnterCriticalSection(&q->lock);
#else
    pthread_mutex_lock(&q->lock);
#endif
}
static void unlock(Queue* q) {
#ifdef _WIN32
    LeaveCriticalSection(&q->lock);
#else
    pthread_mutex_unlock(&q->lock);
#endif
}

/* one chunk of functions, as a code_NNN.o the descriptor is linked with */
static int compile_unit(Queue* q, size_t index, char* error, size_t error_size) {
    Unit* u = &q->units[index];
    char name[32], *file, *path;
    size_t i;
    SnxJit* jit;
    const char* message;
    sprintf(name, "code_%03lu", (unsigned long)index);
    jit = snx_jit_new(q->build->opt);
    if (!jit) {
        snprintf(error, error_size, "out of memory");
        return -1;
    }
    for (i = u->first; i < u->last; ++i) snx_jit_guest_function(jit, q->codegen->functions[i].start, 1);
    for (i = u->first; i < u->last; ++i) snx_jit_emit(jit, q->codegen, &q->codegen->functions[i]);
    if (q->build->keep_ir) {
        char* c_name = snx_format("%s.c", name);
        char* path = snx_path_join(q->build->keep_ir, c_name);
        gcc_jit_context_dump_to_file(jit->ctx, path, 0);
        free(c_name);
        free(path);
    }
    file = snx_format("%s.o", name);
    path = snx_path_join(q->dir, file);
    snprintf(u->object, sizeof(u->object), "%s", path);
    free(file);
    free(path);
    gcc_jit_context_compile_to_file(jit->ctx, GCC_JIT_OUTPUT_KIND_OBJECT_FILE, u->object);
    message = snx_jit_error(jit);
    if (message) snprintf(error, error_size, "%s", message);
    u->fallbacks = jit->fallbacks;
    snx_jit_free(jit);
    return message ? -1 : 0;
}

static void worker(Queue* q) {
    char error[1024], name[32], *label;
    double start, seconds;
    for (;;) {
        size_t i;
        int rc;
        lock(q);
        if (q->failed || q->next == q->count) {
            unlock(q);
            return;
        }
        i = q->next++;
        unlock(q);
        sprintf(name, "code_%03lu", (unsigned long)i);
        error[0] = 0;
        start = snx_seconds();
        rc = compile_unit(q, i, error, sizeof(error));
        seconds = snx_seconds() - start;
        label = rc || q->build->quiet ? NULL : snx_chunk_label(q->codegen, q->units[i].first, q->units[i].last);
        lock(q);
        ++q->done;
        if (rc && !q->failed) {
            q->failed = 1;
            snprintf(q->error, sizeof(q->error), "libgccjit (%s): %.900s", name, error);
        }
        if (label) {
            /* every unit: a line every few seconds or minutes, whichever the machine manages */
            snx_report_unit(q->done, q->count + 1, name, label, 0, seconds); /* + the descriptor */
        }
        unlock(q);
        free(label);
    }
}
#ifdef _WIN32
static DWORD WINAPI thread_main(LPVOID p) {
    worker((Queue*)p);
    return 0;
}
#endif

static void run(Queue* q, unsigned jobs) {
    unsigned i, created = 0;
#ifdef _WIN32
    HANDLE* threads;
    InitializeCriticalSection(&q->lock);
#else
    pthread_mutexattr_t attr;
    pid_t* children;
    fflush(NULL); /* the workers must not flush a copy of the parent's buffered output on their way out */
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED); /* the workers are processes */
    pthread_mutex_init(&q->lock, &attr);
    pthread_mutexattr_destroy(&attr);
#endif
    if (jobs > q->count) jobs = (unsigned)q->count;
    if (jobs < 1) jobs = 1;
#ifdef _WIN32
    threads = snx_alloc(jobs * sizeof(*threads));
    for (i = 1; i < jobs; ++i) {
        HANDLE h = CreateThread(NULL, 0, thread_main, q, 0, NULL);
        if (!h) break;
        threads[created++] = h;
    }
    worker(q);
    for (i = 0; i < created; ++i) {
        WaitForSingleObject(threads[i], INFINITE);
        CloseHandle(threads[i]);
    }
    DeleteCriticalSection(&q->lock);
    free(threads);
#else
    children = (pid_t*)snx_alloc(jobs * sizeof(*children));
    for (i = 1; i < jobs; ++i) {
        pid_t pid = fork();
        if (pid < 0) break;
        if (!pid) {
            worker(q);
            _exit(0); /* not the caller's cleanup: the parent owns the output directory and the exit status */
        }
        children[created++] = pid;
    }
    worker(q);
    for (i = 0; i < created; ++i) {
        int status = 0;
        waitpid(children[i], &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status)) {
            /* a crash in a worker is a build failure, not a hang or a half-linked module */
            lock(q);
            if (!q->failed) {
                q->failed = 1;
                snprintf(q->error, sizeof(q->error), "libgccjit: a compiler process died (status %d)", status);
            }
            unlock(q);
        }
    }
    pthread_mutex_destroy(&q->lock);
    free(children);
#endif
}

/* module.c's emit_module through libgccjit: the same tables and the same snx_module */
static int emit_module(SnxJit* jit, const SnxCodegen* codegen, const SnxCodegenOptions* options,
                       const SnxProgram* loaded, size_t* entry_count, char* error, size_t error_size) {
    gcc_jit_context* x = jit->ctx;
    SnxModuleEntry* entries;
    size_t count, arraysize, i, j, nfns = codegen->function_count ? codegen->function_count : 1;
    gcc_jit_rvalue **values, *interp_address, *index, *e;
    gcc_jit_type *t_fns, *t_u32s, *t_addrs, *t_ptrs, *t_info, *t_mods, *t_id, *t_mod, *t_cstr;
    gcc_jit_field* fields[12];
    gcc_jit_lvalue *fns, *ent_off, *ent_fn, *ent_addr, *ent_ptr, *mods, *mod, *interp_entry, *loop_i;
    gcc_jit_function *interp, *fn;
    gcc_jit_block *start, *head, *body, *done;
    gcc_jit_param* interp_params[2];
    SnxU64 lo = ~(SnxU64)0, hi = 0;
    const SnxNpdm* npdm = &loaded->npdm;
    entries = snx_module_entries(codegen, &count);
    arraysize = count ? count : 1;
    for (i = 0; i < count; ++i)
        if (entries[i].address < SNX_CODE_BASE || entries[i].address - SNX_CODE_BASE > 0xFFFFFFFFUL) {
            free(entries);
            snprintf(error, error_size, "AOT entry exceeds the module ABI's 32-bit offset range");
            return -1;
        }
    values = (gcc_jit_rvalue**)snx_alloc((nfns > arraysize ? nfns : arraysize) * sizeof(*values));
    /* static const GuestFn FNS[] */
    for (i = 0; i < codegen->function_count; ++i)
        values[i] = gcc_jit_context_new_cast(
            x, NULL, gcc_jit_function_get_address(snx_jit_guest_function(jit, codegen->functions[i].start, 0), NULL),
            jit->t_guest_ptr);
    if (!codegen->function_count) values[0] = gcc_jit_context_null(x, jit->t_guest_ptr);
    t_fns = gcc_jit_context_new_array_type(x, NULL, jit->t_guest_ptr, (int)nfns);
    fns = gcc_jit_context_new_global(x, NULL, GCC_JIT_GLOBAL_INTERNAL, t_fns, "FNS");
    gcc_jit_global_set_initializer_rvalue(fns, gcc_jit_context_new_array_constructor(x, NULL, t_fns, nfns, values));
    /* static const u32 ENT_OFF[], ENT_FN[] */
    t_u32s = gcc_jit_context_new_array_type(x, NULL, jit->t_u32, (int)arraysize);
    for (i = 0; i < arraysize; ++i)
        values[i] = jit_u32(jit, i < count ? (SnxU32)(entries[i].address - SNX_CODE_BASE) : 0);
    ent_off = gcc_jit_context_new_global(x, NULL, GCC_JIT_GLOBAL_INTERNAL, t_u32s, "ENT_OFF");
    gcc_jit_global_set_initializer_rvalue(ent_off,
                                          gcc_jit_context_new_array_constructor(x, NULL, t_u32s, arraysize, values));
    for (i = 0; i < arraysize; ++i) values[i] = jit_u32(jit, i < count ? (SnxU32)entries[i].function : 0);
    ent_fn = gcc_jit_context_new_global(x, NULL, GCC_JIT_GLOBAL_INTERNAL, t_u32s, "ENT_FN");
    gcc_jit_global_set_initializer_rvalue(ent_fn,
                                          gcc_jit_context_new_array_constructor(x, NULL, t_u32s, arraysize, values));
    /* static u64 ENT_ADDR[]; static GuestFn ENT_PTR[]; */
    t_addrs = gcc_jit_context_new_array_type(x, NULL, jit->t_u64, (int)arraysize);
    t_ptrs = gcc_jit_context_new_array_type(x, NULL, jit->t_guest_ptr, (int)arraysize);
    ent_addr = gcc_jit_context_new_global(x, NULL, GCC_JIT_GLOBAL_INTERNAL, t_addrs, "ENT_ADDR");
    ent_ptr = gcc_jit_context_new_global(x, NULL, GCC_JIT_GLOBAL_INTERNAL, t_ptrs, "ENT_PTR");
    free(values);
    /* static const SnxModInfo MODS[]: the struct as module_abi.h declares it, laid out by GCC for this host */
    t_cstr = gcc_jit_type_get_pointer(gcc_jit_type_get_const(gcc_jit_context_get_type(x, GCC_JIT_TYPE_CHAR)));
    t_id = gcc_jit_context_new_array_type(x, NULL, jit->t_u8, 32);
    fields[0] = gcc_jit_context_new_field(x, NULL, t_cstr, "name");
    fields[1] = gcc_jit_context_new_field(x, NULL, jit->t_u64, "base");
    fields[2] = gcc_jit_context_new_field(x, NULL, jit->t_u64, "size");
    fields[3] = gcc_jit_context_new_field(x, NULL, t_id, "build_id");
    {
        static const char* const names[8] = {"text_off", "text_size", "ro_off",   "ro_size",
                                             "data_off", "data_size", "bss_size", "is_nro"};
        for (i = 0; i < 8; ++i) fields[4 + i] = gcc_jit_context_new_field(x, NULL, jit->t_u32, names[i]);
    }
    t_info = gcc_jit_struct_as_type(gcc_jit_context_new_struct_type(x, NULL, "SnxModInfo", 12, fields));
    values = (gcc_jit_rvalue**)snx_alloc(loaded->module_count * sizeof(*values));
    for (i = 0; i < loaded->module_count; ++i) {
        const SnxModuleImage* m = &loaded->modules[i];
        gcc_jit_rvalue *f[12], *id[32];
        f[0] = gcc_jit_context_new_cast(x, NULL, gcc_jit_context_new_string_literal(x, m->name), t_cstr);
        f[1] = jit_u64(jit, m->base);
        f[2] = jit_u64(jit, m->size);
        for (j = 0; j < 32; ++j) id[j] = jit_k(jit, jit->t_u8, m->build_id[j]);
        f[3] = gcc_jit_context_new_array_constructor(x, NULL, t_id, 32, id);
        f[4] = jit_u32(jit, m->text_off);
        f[5] = jit_u32(jit, m->text_size);
        f[6] = jit_u32(jit, m->ro_off);
        f[7] = jit_u32(jit, m->ro_size);
        f[8] = jit_u32(jit, m->data_off);
        f[9] = jit_u32(jit, m->data_size);
        f[10] = jit_u32(jit, m->bss);
        f[11] = jit_u32(jit, !!m->is_nro);
        values[i] = gcc_jit_context_new_struct_constructor(x, NULL, t_info, 12, NULL, f);
        if (m->base + m->text_off < lo) lo = m->base + m->text_off;
        if (m->base + m->text_off + m->text_size > hi) hi = m->base + m->text_off + m->text_size;
    }
    t_mods = gcc_jit_context_new_array_type(x, NULL, t_info, (int)loaded->module_count);
    mods = gcc_jit_context_new_global(x, NULL, GCC_JIT_GLOBAL_INTERNAL, t_mods, "MODS");
    gcc_jit_global_set_initializer_rvalue(
        mods, gcc_jit_context_new_array_constructor(x, NULL, t_mods, loaded->module_count, values));
    free(values);
    /* static SnxModule MOD; as 8-byte words, written field by field at module_abi.h's offsets */
    t_mod = gcc_jit_context_new_array_type(x, NULL, jit->t_u64, (int)((sizeof(SnxModule) + 7) / 8));
    mod = gcc_jit_context_new_global(x, NULL, GCC_JIT_GLOBAL_INTERNAL, t_mod, "MOD");
    interp_params[0] = gcc_jit_context_new_param(x, NULL, jit->t_ptr, "c");
    interp_params[1] = gcc_jit_context_new_param(x, NULL, jit->t_u64, "e");
    interp = gcc_jit_context_new_function(x, NULL, GCC_JIT_FUNCTION_IMPORTED, jit->t_void, "snx_interp", 2,
                                          interp_params, 0);
    interp_entry = gcc_jit_context_new_global(x, NULL, GCC_JIT_GLOBAL_IMPORTED, jit->t_guest_ptr, "snx_interp_entry");
    interp_address = gcc_jit_context_new_cast(x, NULL, gcc_jit_function_get_address(interp, NULL), jit->t_guest_ptr);
    /* SNX_EXPORT const SnxModule* snx_module(void) */
    fn = gcc_jit_context_new_function(x, NULL, GCC_JIT_FUNCTION_EXPORTED, jit->t_ptr, "snx_module", 0, NULL, 0);
    jit->fn = fn;
    start = gcc_jit_function_new_block(fn, "entry");
    head = gcc_jit_function_new_block(fn, "head");
    body = gcc_jit_function_new_block(fn, "body");
    done = gcc_jit_function_new_block(fn, "done");
    loop_i = gcc_jit_function_new_local(fn, NULL, jit->t_u32, "i");
    gcc_jit_block_add_assignment(start, NULL, loop_i, jit_u32(jit, 0));
    gcc_jit_block_end_with_jump(start, NULL, head);
    /* for(i=0;i<count;i++){ ENT_ADDR[i]=BASE+ENT_OFF[i]; ENT_PTR[i]=FNS[ENT_FN[i]]; } */
    gcc_jit_block_end_with_conditional(head, NULL,
                                       gcc_jit_context_new_comparison(x, NULL, GCC_JIT_COMPARISON_LT,
                                                                      gcc_jit_lvalue_as_rvalue(loop_i),
                                                                      jit_u32(jit, (SnxU32)count)),
                                       body, done);
    index = gcc_jit_lvalue_as_rvalue(loop_i);
    e = gcc_jit_context_new_binary_op(
        x, NULL, GCC_JIT_BINARY_OP_PLUS, jit->t_u64, jit_u64(jit, SNX_CODE_BASE),
        gcc_jit_context_new_cast(x, NULL,
                                 gcc_jit_lvalue_as_rvalue(gcc_jit_context_new_array_access(
                                     x, NULL, gcc_jit_lvalue_as_rvalue(ent_off), index)),
                                 jit->t_u64));
    gcc_jit_block_add_assignment(
        body, NULL, gcc_jit_context_new_array_access(x, NULL, gcc_jit_lvalue_as_rvalue(ent_addr), index), e);
    gcc_jit_block_add_assignment(body, NULL,
                                 gcc_jit_context_new_array_access(x, NULL, gcc_jit_lvalue_as_rvalue(ent_ptr), index),
                                 gcc_jit_lvalue_as_rvalue(gcc_jit_context_new_array_access(
                                     x, NULL, gcc_jit_lvalue_as_rvalue(fns),
                                     gcc_jit_lvalue_as_rvalue(gcc_jit_context_new_array_access(
                                         x, NULL, gcc_jit_lvalue_as_rvalue(ent_fn), index)))));
    gcc_jit_block_add_assignment_op(body, NULL, loop_i, GCC_JIT_BINARY_OP_PLUS, jit_u32(jit, 1));
    gcc_jit_block_end_with_jump(body, NULL, head);
    /* snx_interp_entry=snx_interp; MOD.abi=...; ... return &MOD; */
    {
        gcc_jit_rvalue* base = gcc_jit_lvalue_get_address(mod, NULL);
        jit->block = done;
        gcc_jit_block_add_assignment(done, NULL, interp_entry, interp_address);
#define FIELD(member, type, value)                                                                                     \
    gcc_jit_block_add_assignment(done, NULL, jit_at(jit, base, offsetof(SnxModule, member), type), value)
        FIELD(abi, jit->t_u32, jit_u32(jit, options->abi));
        FIELD(nmods, jit->t_u32, jit_u32(jit, (SnxU32)loaded->module_count));
        FIELD(mods, jit->t_ptr, gcc_jit_context_new_cast(x, NULL, gcc_jit_lvalue_get_address(mods, NULL), jit->t_ptr));
        FIELD(nentries, jit->t_u32, jit_u32(jit, (SnxU32)count));
        FIELD(entry_addr, jit->t_ptr,
              gcc_jit_context_new_cast(x, NULL, gcc_jit_lvalue_get_address(ent_addr, NULL), jit->t_ptr));
        FIELD(entry_fn, jit->t_ptr,
              gcc_jit_context_new_cast(x, NULL, gcc_jit_lvalue_get_address(ent_ptr, NULL), jit->t_ptr));
        FIELD(interp, jit->t_guest_ptr, interp_address);
        FIELD(code_lo, jit->t_u64, jit_u64(jit, lo));
        FIELD(code_hi, jit->t_u64, jit_u64(jit, hi));
        FIELD(title, jit->t_ptr,
              gcc_jit_context_new_cast(x, NULL,
                                       gcc_jit_context_new_string_literal(x, options->title ? options->title : "Game"),
                                       jit->t_ptr));
        FIELD(title_id, jit->t_u64, jit_u64(jit, npdm->title_id));
        FIELD(main_stack, jit->t_u32, jit_u32(jit, (SnxU32)npdm->stack));
        FIELD(main_prio, jit->t_u32, jit_u32(jit, npdm->prio));
        FIELD(main_core, jit->t_u32, jit_u32(jit, npdm->core));
#undef FIELD
        gcc_jit_block_end_with_return(done, NULL, gcc_jit_context_new_cast(x, NULL, base, jit->t_ptr));
    }
    free(entries);
    *entry_count = count;
    return 0;
}

static int failed(SnxJit* jit, const char* what) {
    const char* message = snx_jit_error(jit);
    if (!message) return 0;
    snx_error("libgccjit (%s): %s", what, message);
    return 1;
}

int snx_build_module_gccjit(SnxProgram* loaded, const SnxCodegenOptions* options, const SnxLlvmBuild* build) {
    SnxCodegen codegen;
    Queue* q = NULL;
    SnxJit* jit = NULL;
    size_t first, last, count, limit, i, entry_count = 0, qsize = 0, usize = 0;
    unsigned long fallbacks = 0;
    char *dir = NULL, *rtlib = NULL, *tmp = NULL, error[1024];
    int rc = -1;
    memset(&codegen, 0, sizeof(codegen));
    if (!loaded->module_count) return snx_error("No modules to generate");
    snx_jit_init(); /* the host target resolves here, before the workers start */
    if (!options->interp_only &&
        snx_nx_codegen_analyze(&codegen, loaded, options->hints, options->hint_count, options->quiet))
        return -1;
    dir = snx_temp_dir(NULL);
    if (!dir) {
        snx_codegen_free(&codegen);
        return snx_error("cannot create a temporary directory");
    }
    rtlib = snx_path_join(dir, "rtlib.o");
    if (snx_write_file(rtlib, snx_rtlib_object, snx_rtlib_object_size)) {
        snx_error("cannot write %s", rtlib);
        goto done;
    }
    limit = options->chunk_instrs ? options->chunk_instrs : SNX_CHUNK_INSTRUCTIONS;
    qsize = sizeof(*q);
    usize = (codegen.function_count + 1) * sizeof(Unit);
    q = (Queue*)shared_alloc(qsize);
    if (!q) {
        snx_error("cannot allocate the compile queue");
        goto done;
    }
    memset(q, 0, qsize);
    q->units = (Unit*)shared_alloc(usize);
    if (!q->units) {
        snx_error("cannot allocate the compile units");
        goto done;
    }
    memset(q->units, 0, usize);
    for (first = 0; first < codegen.function_count; first = last) {
        last = first;
        count = 0;
        do { count += codegen.functions[last++].count; } while (last < codegen.function_count && count < limit);
        q->units[q->count].first = first;
        q->units[q->count++].last = last;
    }
    q->codegen = &codegen;
    q->build = build;
    q->dir = dir;
    if (!options->quiet)
        printf("libgccjit: %lu code unit%s and the module descriptor\n", (unsigned long)q->count,
               q->count == 1 ? "" : "s");
    /* one context per chunk, compiled to an object file: the chunks in parallel, up to build->jobs workers */
    if (q->count) run(q, build->jobs);
    if (q->failed) {
        snx_error("%s", q->error);
        goto done;
    }
    for (i = 0; i < q->count; ++i) fallbacks += q->units[i].fallbacks;
    /* the descriptor, linked by GCC's driver with the chunks and the runtime library */
    jit = snx_jit_new(build->opt);
    if (emit_module(jit, &codegen, options, loaded, &entry_count, error, sizeof(error))) {
        snx_error("libgccjit: %s", error);
        goto done;
    }
    if (build->keep_ir) {
        char* path = snx_path_join(build->keep_ir, "module.c");
        gcc_jit_context_dump_to_file(jit->ctx, path, 0);
        free(path);
    }
    gcc_jit_context_add_driver_option(jit->ctx, rtlib);
    for (i = 0; i < q->count; ++i) gcc_jit_context_add_driver_option(jit->ctx, q->units[i].object);
    tmp = snx_format("%s.tmp", build->output);
    gcc_jit_context_compile_to_file(jit->ctx, GCC_JIT_OUTPUT_KIND_DYNAMIC_LIBRARY, tmp);
    if (failed(jit, "module")) goto done;
    if (snx_replace(tmp, build->output)) {
        snx_error("Linking failed: cannot write %s", build->output);
        goto done;
    }
    if (!options->quiet) {
        printf("libgccjit module: %.1f MB, %lu entries\n", (double)snx_file_size(build->output) / 1000000.0,
               (unsigned long)entry_count);
        if (fallbacks) printf("libgccjit: %lu instructions run through the interpreter step\n", fallbacks);
        puts("note: libgccjit is the backend for DEC Alpha EV67 hosts; elsewhere --backend=llvm (the default) is "
             "the recommended one");
    }
    rc = 0;
done:
    snx_jit_free(jit);
    if (tmp && snx_is_file(tmp)) snx_remove_tree(tmp);
    if (q) shared_free(q->units, usize);
    shared_free(q, qsize);
    if (dir) snx_remove_tree(dir);
    free(dir);
    free(rtlib);
    free(tmp);
    snx_codegen_free(&codegen);
    return rc;
}
#endif
