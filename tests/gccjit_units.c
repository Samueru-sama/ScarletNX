/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* snx_jit_compile_units (gccjit/jit_units.c): the same units compiled one after another in this process and several
   at a time in processes of their own make the same objects; a unit that fails, or whose process dies, fails the
   call with its name; no process is left behind and this process can compile again afterwards. */
#include "common.h"
#include "codegen.h"
#include "jit.h"
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <errno.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#define SNX_UNITS_BASE      0x10000
#define SNX_UNITS_FUNCTIONS 6
#define SNX_UNITS_RET       0xd65f03c0

static int failures;
static void check(int ok, const char* what) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

static unsigned char image[SNX_UNITS_FUNCTIONS * 0x20];
static void put32(size_t offset, SnxU32 word) {
    unsigned i;
    for (i = 0; i < 4; ++i) image[offset + i] = (unsigned char)(word >> (8 * i));
}

typedef struct {
    size_t calls, seen[SNX_UNITS_FUNCTIONS];
    int in_order;
} Done;

static void done(const SnxJitUnits* units, size_t unit, size_t finished, double seconds) {
    Done* d = (Done*)units->user;
    (void)seconds;
    if (finished != d->calls + 1 || unit >= units->count)
        d->in_order = 0;
    else
        ++d->seen[unit];
    ++d->calls;
}

static int compile(SnxCodegen* codegen, const size_t* bounds, char** objects, size_t count, unsigned jobs,
                   const char* keep_ir, SnxJitUnits* u, Done* d) {
    memset(u, 0, sizeof(*u));
    memset(d, 0, sizeof(*d));
    d->in_order = 1;
    u->codegen = codegen;
    u->bounds = bounds;
    u->objects = objects;
    u->count = count;
    u->opt = '1';
    u->jobs = jobs;
    u->keep_ir = keep_ir;
    u->done = done;
    u->user = d;
    return snx_jit_compile_units(u);
}

static int each_once(const Done* d, size_t count) {
    size_t i;
    for (i = 0; i < count; ++i)
        if (d->seen[i] != 1) return 0;
    return d->in_order && d->calls == count;
}

static int no_children(void) {
#ifndef _WIN32
    int status;
    return waitpid(-1, &status, WNOHANG) < 0 && errno == ECHILD;
#else
    return 1;
#endif
}

int main(void) {
    SnxCodeImage view;
    SnxCodegen codegen, broken;
    SnxJitUnits u;
    Done d;
    SnxU64 hints[SNX_UNITS_FUNCTIONS];
    size_t bounds[SNX_UNITS_FUNCTIONS + 1], count, i, k;
    char *root, *here, *forked, *missing, *in_process[SNX_UNITS_FUNCTIONS], *apart[SNX_UNITS_FUNCTIONS], name[32];
    /* six functions of MOV, ADD and RET, the even ones calling the next with BL: calls cross units */
    for (k = 0; k < SNX_UNITS_FUNCTIONS; ++k) {
        size_t at = k * 0x20;
        for (i = 0; i < 8; ++i) put32(at + 4 * i, 0xd503201f); /* NOP */
        put32(at, 0xd2800000 | (SnxU32)(k << 5));              /* MOV X0,#k */
        put32(at + 4, 0x91000c00);                             /* ADD X0,X0,#3 */
        if (k % 2 == 0) {
            put32(at + 8, 0x94000006); /* BL the next function */
            put32(at + 12, SNX_UNITS_RET);
        } else
            put32(at + 8, SNX_UNITS_RET);
        hints[k] = SNX_UNITS_BASE + at;
    }
    memset(&view, 0, sizeof(view));
    view.image = image;
    view.image_size = sizeof(image);
    view.base = SNX_UNITS_BASE;
    view.text_size = sizeof(image);
    memset(&codegen, 0, sizeof(codegen));
    if (snx_codegen_analyze(&codegen, &view, 1, hints, SNX_UNITS_FUNCTIONS, 1)) return snx_error("analysis failed");
    check(codegen.function_count == SNX_UNITS_FUNCTIONS, "one function per hint");
    count = codegen.function_count < SNX_UNITS_FUNCTIONS ? codegen.function_count : SNX_UNITS_FUNCTIONS;
    for (i = 0; i <= count; ++i) bounds[i] = i; /* a function per unit */
    root = snx_temp_dir(NULL);
    if (!root) return 1;
    here = snx_path_join(root, "here");
    forked = snx_path_join(root, "forked");
    missing = snx_path_join(root, "missing");
    check(!snx_mkdirs(here) && !snx_mkdirs(forked), "object folders");
    for (i = 0; i < count; ++i) {
        sprintf(name, "code_%03lu.o", (unsigned long)i);
        in_process[i] = snx_path_join(here, name);
        apart[i] = snx_path_join(forked, name);
    }

    /* three processes at a time for six units, then this process: the lock is free again after the forks */
    check(!compile(&codegen, bounds, apart, count, 3, NULL, &u, &d), u.error);
    check(each_once(&d, count), "every forked unit reported once, finished counting up");
    check(no_children(), "no unit process left after a forked compile");
    check(!compile(&codegen, bounds, in_process, count, 1, NULL, &u, &d), u.error);
    check(each_once(&d, count), "every unit reported once, in this process");
    for (i = 0; i < count; ++i) {
        size_t a_size = 0, b_size = 0;
        unsigned char *a = snx_read_file(in_process[i], &a_size), *b = snx_read_file(apart[i], &b_size);
        check(a && b && a_size && a_size == b_size && !memcmp(a, b, a_size), "the same object from either way");
        free(a);
        free(b);
    }
    check(!compile(&codegen, bounds, apart, 0, 3, NULL, &u, &d) && !d.calls, "no units, nothing to do");

    /* a dump into a folder that doesn't exist: every unit fails in libgccjit, either way */
    check(compile(&codegen, bounds, apart, count, 3, missing, &u, &d) && !strncmp(u.error, "libgccjit (code_", 16),
          "a failing unit fails the forked compile with its name");
    check(!d.calls, "no failed unit reported as compiled");
    check(no_children(), "no unit process left after a failure");
    check(compile(&codegen, bounds, in_process, count, 1, missing, &u, &d) &&
              !strncmp(u.error, "libgccjit (code_000)", 20),
          "a failing unit fails the compile in this process with its name");
#ifndef _WIN32
    /* a unit's process that dies (no function table: SIGSEGV) fails the call, and only its process dies */
    {
        struct rlimit none;
        none.rlim_cur = none.rlim_max = 0;
        setrlimit(RLIMIT_CORE, &none);
    }
    broken = codegen;
    broken.functions = NULL;
    check(compile(&broken, bounds, apart, count, 64, NULL, &u, &d) && !strncmp(u.error, "libgccjit (code_", 16) &&
              strstr(u.error, "its process") != NULL,
          "a unit whose process dies fails the compile with its name");
    if (!strstr(u.error, "its process")) fprintf(stderr, "error: %s\n", u.error);
    check(no_children(), "no unit process left after one died");
    /* a failure stops the units still compiling: one fails at once (its object in a folder that doesn't exist), the
       other would wait for ever to write its object into a FIFO nobody reads; the call ends with the first one's error */
    {
        char *fifo = snx_path_join(root, "fifo.o"), *objects[2];
        check(!mkfifo(fifo, 0600), "a FIFO");
        objects[0] = snx_path_join(missing, "code_000.o");
        objects[1] = fifo;
        alarm(60); /* a unit left running would hang the call: the test ends here instead, failed */
        check(compile(&codegen, bounds, objects, 2, 2, NULL, &u, &d) && !strncmp(u.error, "libgccjit (code_000)", 20),
              "a failing unit stops the units still compiling");
        alarm(0);
        check(no_children(), "no unit process left after the others were stopped");
        free(objects[0]);
        free(fifo);
    }
#else
    (void)broken;
#endif

    for (i = 0; i < count; ++i) {
        free(in_process[i]);
        free(apart[i]);
    }
    free(here);
    free(forked);
    free(missing);
    check(!snx_remove_tree(root), "cleanup");
    free(root);
    snx_codegen_free(&codegen);
    printf("gccjit units: %s\n", failures ? "FAILED" : "passed");
    return failures != 0;
}
