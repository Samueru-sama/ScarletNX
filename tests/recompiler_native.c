/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Synthetic NRO -> CLI -> C -> DSO -> AOT/interpreter execution. No game data.
   With a fourth argument, the backend: llvm runs the same passes through --backend=llvm (no C, the module written by
   the recompiler), where pass 3 has no compiler to fail. */
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

static int failures;
static void check(int ok, const char* what) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}
static void environment(const char* name, const char* value) {
#ifdef _WIN32
    check(!_putenv_s(name, value ? value : ""), "set test environment");
#else
    check(!(value ? setenv(name, value, 1) : unsetenv(name)), "set test environment");
#endif
}
static void check_empty(const char* dir) {
    char** names = NULL;
    size_t count = 0;
    check(!snx_list_dir(dir, &names, &count) && !count, "temporary build directory cleaned up");
    snx_free_strings(names, count);
}
static const char* backend;
static void check_sources(const char* dir, const char* suffix) {
    char** names = NULL;
    size_t count = 0, i, len;
    check(!snx_list_dir(dir, &names, &count) && count, "generated C retained on request");
    for (i = 0; i < count; ++i) {
        len = strlen(names[i]);
        check(len > strlen(suffix) && !strcmp(names[i] + len - strlen(suffix), suffix),
              "only generated sources retained");
    }
    snx_free_strings(names, count);
}
static void put32(unsigned char* p, size_t off, unsigned long n) {
    unsigned i;
    for (i = 0; i < 4; ++i) p[off + i] = (unsigned char)(n >> (8 * i));
}
static void undefined(CPU* c, u64 pc, u32 word) {
    (void)c;
    (void)pc;
    (void)word;
    check(0, "unexpected UNDEF");
}
static void execute_module(const char* path, const unsigned char* image, int interp) {
    typedef const SnxModule* (*ModuleFn)(void);
    ModuleFn get;
    const SnxModule* mod;
    CPU cpu;
    RtAPI rt;
    size_t i;
#ifdef _WIN32
    HMODULE handle = LoadLibraryA(path);
    check(handle != NULL, "load generated DLL");
    if (!handle) return;
    get = (ModuleFn)(void*)GetProcAddress(handle, "snx_module");
#else
    void* handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) fprintf(stderr, "%s\n", dlerror());
    check(handle != NULL, "load generated DSO");
    if (!handle) return;
    get = (ModuleFn)dlsym(handle, "snx_module");
#endif
    check(get != NULL, "module export");
    if (!get) goto done;
    mod = get();
    check(mod->abi == SNX_ABI && mod->nmods == 1, "module ABI/metadata");
    check(interp ? mod->nentries == 0 : mod->nentries >= 1, "AOT/interpreter entry tables");
    memset(&cpu, 0, sizeof(cpu));
    memset(&rt, 0, sizeof(rt));
    rt.undef = undefined;
    cpu.rt = &rt;
    cpu.code_lo = mod->code_lo;
    cpu.code_hi = mod->code_hi;
    cpu.disp = calloc((size_t)(mod->code_hi - mod->code_lo) / 4, sizeof(*cpu.disp));
    cpu.mem = calloc(1, (size_t)mod->mods[0].base + 0x1000);
    check(cpu.mem && cpu.disp, "guest fixture allocation");
    if (cpu.mem && cpu.disp) {
        memcpy(cpu.mem + mod->mods[0].base, image, 0x200);
        for (i = 0; i < mod->nentries; ++i) cpu.disp[(mod->entry_addr[i] - mod->code_lo) / 4] = mod->entry_fn[i];
        if (interp)
            mod->interp(&cpu, mod->mods[0].base);
        else {
            check(cpu.disp[0] != NULL, "entry point discovered");
            if (cpu.disp[0]) cpu.disp[0](&cpu, mod->mods[0].base);
        }
        check(cpu.x[0] == 42 && cpu.pc == 0, "MOV/ADD/RET result");
    }
    free(cpu.mem);
    free(cpu.disp);
done:
#ifdef _WIN32
    FreeLibrary(handle);
#else
    dlclose(handle);
#endif
}
int main(int argc, char** argv) {
    unsigned char image[0x200];
    const char* args[24];
    char *root, *input, *out, *self, *module, *text = NULL, *data_file;
    char *cached, *source, *nested, *temp, *build, *old_flags;
    size_t cached_size;
    int rc, pass, k;
    if (argc != 4 && argc != 5) return 2;
    backend = argc == 5 ? argv[4] : NULL;
    root = snx_temp_dir(NULL);
    check(root != NULL, "temp fixture");
    if (!root) return 1;
    input = snx_path_join(root, "program with spaces.nro");
    out = snx_path_join(root, "output with spaces");
    self = snx_executable(argv[0]);
    temp = snx_path_join(root, "temporary files with spaces");
    build = snx_path_join(out, "Native Fixture_build");
    check(!snx_mkdirs(temp), "temporary build parent");
#ifdef _WIN32
    environment("TMP", temp);
    environment("TEMP", temp);
#else
    environment("TMPDIR", temp);
#endif
    old_flags = getenv("SNX_CFLAGS") ? snx_strdup(getenv("SNX_CFLAGS")) : NULL;
    environment("SNX_CFLAGS", NULL);
    memset(image, 0, sizeof(image));
    put32(image, 0, 0x14000020); /* B +0x80 */
    memcpy(image + 0x10, "NRO0", 4);
    put32(image, 0x18, sizeof(image));
    put32(image, 0x20, 0);
    put32(image, 0x24, 0x100);
    put32(image, 0x28, 0x100);
    put32(image, 0x2c, 0x80);
    put32(image, 0x30, 0x180);
    put32(image, 0x34, 0x80);
    put32(image, 0x80, 0xd28004e0); /* MOV X0,#39 */
    put32(image, 0x84, 0x91000c00); /* ADD X0,X0,#3 */
    put32(image, 0x88, 0xd65f03c0); /* RET */
    check(!snx_write_file(input, image, sizeof(image)), "write NRO");
    for (pass = 0; pass < 6; ++pass) {
        if (pass == 1) {
            data_file = snx_path_join(out, "Native Fixture_data/romfs.bin");
            check(!snx_write_file(data_file, "old RomFS", 9), "stale NRO assets fixture");
            free(data_file);
        }
        k = 0;
        args[k++] = argv[1];
        args[k++] = "--repo";
        args[k++] = argv[2];
        args[k++] = "-r";
        args[k++] = input;
        args[k++] = "Native Fixture";
        args[k++] = "--outdir";
        args[k++] = out;
        if (backend) {
            args[k++] = "--backend";
            args[k++] = backend;
        } else {
            args[k++] = "--cc";
            args[k++] = argv[3];
        }
        args[k++] = "--runtime";
        args[k++] = self;
        args[k++] = "--jobs";
        args[k++] = "2";
        args[k++] = "--chunk";
        args[k++] = "2";
        if (pass == 2) args[k++] = "--interp-only";
        if (pass == 5) args[k++] = "--keep-c";
        args[k] = NULL;
        if (pass == 3) environment("SNX_CFLAGS", "-fsnx-invalid-test-option");
        rc = snx_run(args, root, NULL, &text);
        if (rc && ((pass != 3 || backend) && pass != 4)) fprintf(stderr, "%s\n", text);
        if ((pass == 3 && !backend) || pass == 4)
            check(rc != 0, pass == 3 ? "compiler failure" : "link output replacement failure");
        else
            check(!rc, pass == 2 ? "interpreter CLI build" : "AOT CLI build");
        environment("SNX_CFLAGS", NULL);
        data_file = snx_path_join(out, "Native Fixture.abi");
        check(snx_is_file(data_file) == !rc, "ABI checkpoint published only after a successful build");
        free(data_file);
        if (pass == 1) {
            data_file = snx_path_join(out, "Native Fixture_data/romfs.bin");
            check(!snx_is_file(data_file), "asset-free NRO removes stale RomFS");
            free(data_file);
        }
        check_empty(temp);
        if (pass == 5)
            check_sources(build, backend ? ".ll" : ".c");
        else
            check(!snx_is_dir(build), "no persistent build directory by default");
        free(text);
        text = NULL;
#ifdef _WIN32
        module = snx_path_join(out, "Native Fixture_code.dll");
#else
        module = snx_path_join(out, "Native Fixture_code.so");
#endif
        if (!rc) execute_module(module, image, pass == 2);
        /* Force a link publish failure without relying on compiler-specific flags. */
        if (pass == 3) {
            check(!snx_remove_tree(module), "remove previous module");
            check(!snx_mkdirs(module), "block link output with a directory");
        } else if (pass == 4)
            check(!snx_remove_tree(module), "remove link output blocker");
        free(module);
    }
    check(!snx_remove_tree(build), "remove kept sources before no-build test");
    environment("SNX_CFLAGS", old_flags);
    free(old_flags);
    data_file = snx_path_join(out, "Native Fixture_data/exefs/main.nro");
    cached = (char*)snx_read_file(data_file, &cached_size);
    check(cached && cached_size == sizeof(image) && !memcmp(cached, image, sizeof(image)), "NRO staging");
    free(cached);
    free(data_file);
    /* A previously staged NSO must not silently shadow the new NRO. */
    data_file = snx_path_join(out, "Native Fixture_data/exefs/main");
    check(!snx_write_file(data_file, "old NSO", 7), "conflicting format fixture");
    k = 0;
    args[k++] = argv[1];
    args[k++] = "--repo";
    args[k++] = argv[2];
    args[k++] = "--recomp";
    args[k++] = input;
    args[k++] = "--out=Native Fixture";
    args[k++] = "--outdir";
    args[k++] = out;
    args[k++] = "--no-build";
    args[k++] = "-O2";
    args[k] = NULL;
    check(snx_run(args, root, NULL, &text) != 0, "reject mixed NSO/NRO cache");
    check_empty(temp);
    free(text);
    text = NULL;
    check(!snx_remove_tree(data_file), "remove conflict fixture");
    free(data_file);
    check(!snx_remove_tree(build), "remove sources from failed no-build");
    check(!snx_run(args, root, NULL, &text), "legacy aliases and attached optimization level");
    check_sources(build, ".c");
    check_empty(temp);
    free(text);
    text = NULL;
    k = 0;
    args[k++] = argv[1];
    args[k++] = "--repo";
    args[k++] = argv[2];
    args[k++] = "-r";
    args[k++] = input;
    args[k++] = "--jobs";
    args[k++] = "0";
    args[k] = NULL;
    check(snx_run(args, root, NULL, &text) != 0, "reject zero jobs");
    free(text);
    source = snx_path_join(root, "copy source");
    nested = snx_path_join(source, "nested/output");
    check(!snx_mkdirs(source), "copy source fixture");
    check(snx_copy_tree(source, nested) != 0 && !snx_is_dir(nested), "reject recursive output inside source");
    free(source);
    free(nested);
    free(input);
    free(out);
    free(self);
    free(temp);
    free(build);
    check(!snx_remove_tree(root), "fixture cleanup");
    free(root);
    printf("Native recompiler: %s\n", failures ? "FAILED" : "passed");
    return failures != 0;
}
