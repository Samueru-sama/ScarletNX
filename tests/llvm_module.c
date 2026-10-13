/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A whole program through both backends: a synthetic NRO with a loop, BL/RET, an indirect branch inside its function
   (the D_ dispatch), a tail call into another function, SVC, exclusives, FP and SIMD, recompiled by the CLI with
   --backend=portable and --backend=llvm in small chunks (calls cross units). Both modules run it from the same state,
   AOT and through their interpreter; every register, the flags, FP state, stack memory and the SVC calls must match.
     llvm_module SCARLETNX REPO CLANG */
#include "common.h"
#include "cpu.h"
#include "loader/module_abi.h"
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

/* assembled with clang -target aarch64-linux-gnu: main at 0x80 (reached by the B at 0), func, tail at 0x200 */
static const u32 program[] = {0xD2800000, 0xD2800181, 0x1E6F1001, 0x6F00E402, 0x94000016, 0x9E620003, 0x1E632800,
                              0x1E610800, 0x4EA28442, 0x4E040C24, 0x4EA48442, 0xF1000421, 0x54FFFF01, 0x10000062,
                              0xD61F0040, 0xD4200020, 0xF81F0FE0, 0xA8C113E3, 0xB4000043, 0x910FA000, 0x885FFFE7,
                              0x8808FFE0, 0x37180040, 0xD2401C00, 0xAA1F03FE, 0x14000047, 0x8B010000, 0xD37FF804,
                              0xCA440C00, 0x9B011005, 0x9AC108A6, 0x9A868000, 0xD65F03C0};
static const u32 tail[] = {0xAA0003E6, 0xD40000E1, 0x9E780009, 0xD65F03C0};

#define IMAGE  0x400
#define MEMORY 0x10000

static int failures;
static void check(int ok, const char* what) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}
static void put32(unsigned char* p, size_t off, unsigned long n) {
    unsigned i;
    for (i = 0; i < 4; ++i) p[off + i] = (unsigned char)(n >> (8 * i));
}

typedef struct {
    u64 imm[8], x0[8];
    int count, undefined;
} Calls;
static Calls* calls;
static void svc(CPU* c, u32 imm) {
    if (calls->count < 8) {
        calls->imm[calls->count] = imm;
        calls->x0[calls->count++] = c->x[0];
    }
}
static void undefined(CPU* c, u64 pc, u32 w) {
    (void)c;
    (void)pc;
    (void)w;
    ++calls->undefined;
}

typedef struct {
    CPU cpu;
    unsigned char memory[MEMORY];
    Calls calls;
} Run;

static int run(const char* path, const unsigned char* image, int interp, Run* out) {
    typedef const SnxModule* (*ModuleFn)(void);
    ModuleFn get;
    const SnxModule* mod;
    RtAPI rt;
    unsigned char* memory;
    size_t i;
#ifdef _WIN32
    HMODULE handle = LoadLibraryA(path);
    get = handle ? (ModuleFn)(void*)GetProcAddress(handle, "snx_module") : NULL;
#else
    void* handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) fprintf(stderr, "%s\n", dlerror());
    get = handle ? (ModuleFn)dlsym(handle, "snx_module") : NULL;
#endif
    if (!get) return -1;
    mod = get();
    memset(out, 0, sizeof(*out));
    memset(&rt, 0, sizeof(rt));
    rt.svc = svc;
    rt.undef = undefined;
    calls = &out->calls;
    memory = (unsigned char*)calloc(1, (size_t)mod->mods[0].base + MEMORY);
    out->cpu.disp = (GuestFn*)calloc((size_t)(mod->code_hi - mod->code_lo) / 4, sizeof(GuestFn));
    memcpy(memory + mod->mods[0].base, image, IMAGE);
    for (i = 0; i < mod->nentries; ++i) out->cpu.disp[(mod->entry_addr[i] - mod->code_lo) / 4] = mod->entry_fn[i];
    out->cpu.mem = memory;
    out->cpu.rt = &rt;
    out->cpu.code_lo = mod->code_lo;
    out->cpu.code_hi = mod->code_hi;
    out->cpu.x[31] = mod->mods[0].base + MEMORY / 2;
    out->cpu.x[30] = 0xDEAD0000;
    if (interp)
        mod->interp(&out->cpu, mod->mods[0].base);
    else if (out->cpu.disp[0])
        out->cpu.disp[0](&out->cpu, mod->mods[0].base);
    memcpy(out->memory, memory + mod->mods[0].base, MEMORY);
    free(memory);
    free(out->cpu.disp);
#ifdef _WIN32
    FreeLibrary(handle);
#else
    dlclose(handle);
#endif
    return 0;
}

static int same(const Run* a, const Run* b) {
    return !memcmp(a->cpu.x, b->cpu.x, sizeof(a->cpu.x)) && a->cpu.pc == b->cpu.pc && a->cpu.n == b->cpu.n &&
           a->cpu.z == b->cpu.z && a->cpu.cf == b->cpu.cf && a->cpu.vf == b->cpu.vf &&
           !memcmp(a->cpu.v, b->cpu.v, sizeof(a->cpu.v)) && a->cpu.fpsr == b->cpu.fpsr &&
           a->cpu.excl_on == b->cpu.excl_on && !memcmp(a->memory, b->memory, MEMORY) &&
           !memcmp(&a->calls, &b->calls, sizeof(a->calls));
}

int main(int argc, char** argv) {
    static const char* const backends[2] = {"portable", "llvm"};
    static Run runs[2][2];
    unsigned char image[IMAGE];
    char *root, *input, *self, *out[2], *module[2], *text = NULL;
    const char* args[24];
    int b, k, interp;
    size_t i;
    if (argc != 4) return 2;
    root = snx_temp_dir(NULL);
    if (!root) return 1;
    memset(image, 0, sizeof(image));
    put32(image, 0, 0x14000020); /* B main */
    memcpy(image + 0x10, "NRO0", 4);
    put32(image, 0x18, IMAGE);
    put32(image, 0x24, 0x300);
    put32(image, 0x28, 0x300);
    put32(image, 0x2c, 0x80);
    put32(image, 0x30, 0x380);
    put32(image, 0x34, 0x80);
    for (i = 0; i < sizeof(program) / sizeof(program[0]); ++i) put32(image, 0x80 + 4 * i, program[i]);
    for (i = 0; i < sizeof(tail) / sizeof(tail[0]); ++i) put32(image, 0x200 + 4 * i, tail[i]);
    input = snx_path_join(root, "program.nro");
    self = snx_executable(argv[0]);
    check(!snx_write_file(input, image, sizeof(image)), "write NRO");
    for (b = 0; b < 2; ++b) {
        out[b] = snx_path_join(root, backends[b]);
#ifdef _WIN32
        module[b] = snx_path_join(out[b], "Program_code.dll");
#else
        module[b] = snx_path_join(out[b], "Program_code.so");
#endif
        k = 0;
        args[k++] = argv[1];
        args[k++] = "--repo";
        args[k++] = argv[2];
        args[k++] = "-r";
        args[k++] = input;
        args[k++] = "Program";
        args[k++] = "--outdir";
        args[k++] = out[b];
        args[k++] = "--runtime";
        args[k++] = self;
        args[k++] = "--chunk";
        args[k++] = "4";
        args[k++] = "--backend";
        args[k++] = backends[b];
        if (!b) {
            args[k++] = "--cc";
            args[k++] = argv[3];
        }
        args[k++] = "-q";
        args[k] = NULL;
        if (snx_run(args, root, NULL, &text)) {
            fprintf(stderr, "%s\n", text ? text : "");
            check(0, b ? "LLVM build" : "portable build");
        }
        free(text);
        text = NULL;
        for (interp = 0; interp < 2; ++interp)
            check(!run(module[b], image, interp, &runs[b][interp]), interp ? "interpreter run" : "AOT run");
    }
    check(runs[0][0].cpu.pc == 0 && runs[0][0].calls.count == 1 && runs[0][0].calls.imm[0] == 7 &&
              !runs[0][0].calls.undefined,
          "the program returns to 0 after one SVC #7");
    check(same(&runs[0][0], &runs[0][1]), "portable AOT and interpreter agree");
    check(same(&runs[0][0], &runs[1][0]), "LLVM AOT matches portable AOT");
    check(same(&runs[0][0], &runs[1][1]), "LLVM interpreter matches portable AOT");
    printf("x0=%llx x9=%llx d0=%llx v2=%llx:%llx\n", (unsigned long long)runs[1][0].cpu.x[0],
           (unsigned long long)runs[1][0].cpu.x[9], (unsigned long long)runs[1][0].cpu.v[0].d[0],
           (unsigned long long)runs[1][0].cpu.v[2].d[1], (unsigned long long)runs[1][0].cpu.v[2].d[0]);
    check(!snx_remove_tree(root), "fixture cleanup");
    printf("LLVM module: %s\n", failures ? "FAILED" : "passed");
    return failures != 0;
}
