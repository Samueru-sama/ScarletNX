/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later (LICENSE). */
#define _POSIX_C_SOURCE 200809L
#include "recompile.h"
#include "loader.h"
#include "module.h"
#include "toolchain.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static void usage(void) {
    puts("Usage: scarletnx -r INPUT [NAME] [OPTIONS]\n"
         "  INPUT                  Game/ExeFS directory or .nro\n"
         "  -o, --name, --out NAME  Output name (default: NPDM or input name)\n"
         "  --outdir DIR           Output directory (default: checkout/games)\n"
         "  --cc CLANG             Compiler executable\n"
         "  -O LEVEL               Optimization: 0,1,2,3,s,z (default: 2)\n"
         "  --jobs N               Parallel compiles (default: CPU count)");
    puts("  --chunk N              Instructions per C chunk (default: 30000)\n"
         "  --target auto|linux|win64\n"
         "  --native               Compile for this host CPU\n"
         "  --no-build             Generate C/data/INI only\n"
         "  --interp-only          Generate fallback interpreter only\n"
         "  --runtime FILE         Deploy this runtime instead of building one\n"
         "  --keep-c               Save generated C in <outdir>/<NAME>_build\n"
         "  -q, --quiet            Suppress progress");
    puts("  --hook-direct-calls    Route direct guest calls through dispatch for runtime instrumentation");
    puts("Hints: <outdir>/<NAME>.hints (hex PCs, one per line).\n"
         "SNX_CFLAGS adds whitespace-separated compiler arguments.");
}
static unsigned number(const char* text, unsigned max) {
    char* end;
    unsigned long n;
    errno = 0;
    n = strtoul(text, &end, 10);
    return errno || !*text || *end || n == 0 || n > max ? 0 : (unsigned)n;
}
static char* title_name(const char* name, const SnxProgram* p, const char* input) {
    const char *start, *s;
    char *out, *dot;
    size_t n = 0;
    int input_name = 0;
    if (!name && p->npdm.name[0] && strcmp(p->npdm.name, "Application")) name = p->npdm.name;
    if (!name) {
        start = input;
        for (s = input; *s; ++s)
            if (*s == '/' || *s == '\\') start = s + 1;
        name = start;
        input_name = 1;
    }
    out = (char*)snx_alloc(strlen(name) + 1);
    /* Preserve UTF-8 names; exclude path delimiters and C/OS metacharacters. */
    for (s = name; *s; ++s)
        if ((unsigned char)*s >= 128 || isalnum((unsigned char)*s) || *s == '-' || *s == '_' || *s == ' ' ||
            (input_name && *s == '.'))
            out[n++] = *s;
    out[n] = 0;
    if (input_name && (dot = strrchr(out, '.')) != NULL) *dot = 0;
    n = strlen(out);
    while (n && (out[n - 1] == ' ' || out[n - 1] == '.')) out[--n] = 0;
    if (!n) {
        free(out);
        return snx_strdup("Game");
    }
    return out;
}
unsigned snx_read_abi(const SnxPaths* paths) {
    char* path = snx_path_join(paths->repo, "src/common/cpu/aarch64/cpu.h");
    unsigned char* data;
    const char* p;
    size_t n;
    unsigned abi = 0;
    data = snx_read_file(path, &n);
    free(path);
    if (!data) return 0;
    p = strstr((const char*)data, "#define SNX_ABI ");
    if (p) abi = (unsigned)strtoul(p + strlen("#define SNX_ABI "), NULL, 10);
    free(data);
    return abi;
}
static SnxU64* read_hints(const char* base, size_t* count) {
    char *path = snx_format("%s.hints", base), line[256], *s;
    FILE* f = snx_fopen(path, "r");
    SnxU64 *hints = NULL, value;
    size_t n = 0;
    int digits, invalid, c;
    free(path);
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            value = 0;
            digits = 0;
            invalid = 0;
            s = line;
            while (isspace((unsigned char)*s)) ++s;
            if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
            while (isxdigit((unsigned char)*s)) {
                if (++digits > 16) invalid = 1;
                c = tolower((unsigned char)*s++);
                value = (value << 4) | (SnxU64)(c <= '9' ? c - '0' : c - 'a' + 10);
            }
            while (isspace((unsigned char)*s)) ++s;
            if (!invalid && digits && !*s && !(value & 3)) {
                hints = (SnxU64*)snx_realloc(hints, (n + 1) * sizeof(*hints));
                hints[n++] = value;
            }
        }
        fclose(f);
    }
    *count = n;
    return hints;
}
static int copy_data(const SnxProgram* p, const char* dir, int quiet) {
    char *ex = snx_path_join(dir, "exefs"), *dst, *src, **names;
    size_t n, i;
    int rc = 0;
    if (snx_is_link(ex) || snx_mkdirs(ex)) {
        free(ex);
        return 1;
    }
    /* A cached mount of the other format can take precedence in the runtime. */
    dst = snx_path_join(ex, p->modules[0].is_nro ? "main" : "main.nro");
    if (snx_is_file(dst)) {
        snx_error("cached data has a different format; use another name or remove %s/exefs and its RomFS cache", dir);
        free(dst);
        free(ex);
        return 1;
    }
    free(dst);
    if (!p->modules[0].is_nro) {
        dst = snx_path_join(dir, "romfs.bin");
        if (snx_is_file(dst)) {
            snx_error("cached romfs.bin conflicts with NSO input; remove that cache before recompiling");
            free(dst);
            free(ex);
            return 1;
        }
        free(dst);
    }
    if (p->module_count == 1 && p->modules[0].is_nro) {
        dst = snx_path_join(ex, "main.nro");
        rc = snx_copy_file(p->entry_path, dst);
        free(dst);
        if (!rc) {
            dst = snx_path_join(dir, "romfs.bin");
            if (p->romfs_data)
                rc = snx_write_file(dst, p->romfs_data, p->romfs_size);
            else if (snx_is_file(dst))
                /* An updated NRO without embedded RomFS must not inherit the old image's assets. */
                rc = snx_remove_tree(dst);
            free(dst);
        }
    } else {
        if (snx_list_dir(p->exefs_path, &names, &n)) {
            free(ex);
            return 1;
        }
        for (i = 0; i < n && !rc; ++i) {
            src = snx_path_join(p->exefs_path, names[i]);
            dst = snx_path_join(ex, names[i]);
            if (snx_is_link(src) || snx_is_link(dst))
                rc = 1;
            else if (snx_is_file(src))
                rc = snx_copy_file(src, dst);
            free(src);
            free(dst);
        }
        snx_free_strings(names, n);
        if (!rc && p->romfs_path) {
            dst = snx_path_join(dir, "romfs");
            if (!snx_is_dir(dst)) {
                /* A stopped copy must not look like a complete cache on the next run. */
                char* pending = snx_format("%s.copying", dst);
                if (!quiet) puts("Copying RomFS...");
                rc = ((snx_is_dir(pending) || snx_is_file(pending) || snx_is_link(pending)) &&
                      snx_remove_tree(pending)) ||
                     snx_copy_tree(p->romfs_path, pending) || snx_replace(pending, dst);
                free(pending);
            }
            free(dst);
        }
    }
    if (!rc && !p->npdm_data) {
        dst = snx_path_join(ex, "main.npdm");
        if (snx_is_file(dst)) rc = snx_remove_tree(dst);
        free(dst);
    }
    free(ex);
    return rc;
}
static int save_sources(const SnxSources* sources, const char* base) {
    char *dir = snx_format("%s_build", base), *dst;
    const char *name, *p;
    size_t i;
    int rc = 1;
    if (snx_is_link(dir) || snx_mkdirs(dir)) goto done;
    for (i = 0; i < sources->count; ++i) {
        name = sources->paths[i];
        for (p = name; *p; ++p)
            if (*p == '/' || *p == '\\') name = p + 1;
        dst = snx_path_join(dir, name);
        rc = snx_is_link(dst) || snx_copy_file(sources->paths[i], dst);
        free(dst);
        if (rc) goto done;
    }
    rc = 0;
done:
    if (rc) snx_error("cannot save generated C in %s", dir);
    free(dir);
    return rc;
}
int snx_recompile(int argc, char** argv, const SnxPaths* paths) {
    const char *input = NULL, *name = NULL, *outdir = NULL, *runtime = NULL, *target = "auto", *arg, *value;
    SnxCompileOptions compile;
    SnxCodegenOptions gen;
    SnxProgram program;
    SnxSources sources;
    char *title = NULL, *out = NULL, *base = NULL, *build = NULL, *data = NULL, *code = NULL;
    char *ini = NULL, *ini_text = NULL, *run = NULL, *exe = NULL, *tmp = NULL, *romfs = NULL;
    char error[1024], option[64];
    const char* equal;
    size_t option_size;
    int i, rc = 1, no_build = 0, keep_c = 0, host_win = 0, positional = 0;
    memset(&compile, 0, sizeof(compile));
    memset(&gen, 0, sizeof(gen));
    memset(&program, 0, sizeof(program));
    memset(&sources, 0, sizeof(sources));
    compile.opt = "2";
    compile.jobs = snx_cpu_count();
    gen.chunk_instrs = 30000;
#ifdef _WIN32
    host_win = 1;
#endif
    compile.target_win = host_win;
    for (i = 0; i < argc; ++i) {
        arg = argv[i];
        if (positional) {
            if (!input)
                input = arg;
            else if (!name)
                name = arg;
            else
                return snx_error("unexpected argument: %s", arg);
            continue;
        }
        if (!strcmp(arg, "--help") || !strcmp(arg, "-h")) {
            usage();
            return 0;
        }
        if (!strcmp(arg, "--")) {
            positional = 1;
            continue;
        }
        if (!strcmp(arg, "--no-build"))
            no_build = 1;
        else if (!strcmp(arg, "--interp-only"))
            gen.interp_only = 1;
        else if (!strcmp(arg, "--hook-direct-calls"))
            gen.hook_direct_calls = 1;
        else if (!strcmp(arg, "--keep-c"))
            keep_c = 1;
        else if (!strcmp(arg, "--quiet") || !strcmp(arg, "-q"))
            compile.quiet = gen.quiet = 1;
        else if (arg[0] == '-') {
            value = NULL;
            if (arg[1] == 'O' && arg[2]) {
                value = arg + 2;
                arg = "-O";
            } else if ((equal = strchr(arg, '=')) != NULL) {
                option_size = (size_t)(equal - arg);
                if (option_size >= sizeof(option)) return snx_error("unknown recompiler option: %s", arg);
                memcpy(option, arg, option_size);
                option[option_size] = 0;
                value = equal + 1;
                arg = option;
            }
            if (strcmp(arg, "-o") && strcmp(arg, "--name") && strcmp(arg, "--out") && strcmp(arg, "--outname") &&
                strcmp(arg, "--outdir") && strcmp(arg, "--runtime") && strcmp(arg, "--target") && strcmp(arg, "--cc") &&
                strcmp(arg, "-O") && strcmp(arg, "--jobs") && strcmp(arg, "--chunk"))
                return snx_error("unknown recompiler option: %s", arg);
            if (!value) {
                if (++i == argc) return snx_error("%s requires a value", arg);
                value = argv[i];
            }
            if (!*value) return snx_error("%s requires a nonempty value", arg);
            if (!strcmp(arg, "-o") || !strcmp(arg, "--name") || !strcmp(arg, "--out") || !strcmp(arg, "--outname"))
                name = value;
            else if (!strcmp(arg, "--outdir"))
                outdir = value;
            else if (!strcmp(arg, "--runtime"))
                runtime = value;
            else if (!strcmp(arg, "--target"))
                target = value;
            else if (!strcmp(arg, "--cc"))
                compile.cc = value;
            else if (!strcmp(arg, "-O")) {
                if (strlen(value) != 1 || !strchr("0123sz", value[0])) return snx_error("-O expects 0,1,2,3,s,z");
                compile.opt = value;
            } else if (!strcmp(arg, "--jobs")) {
                compile.jobs = number(value, 1024);
                if (!compile.jobs) return snx_error("--jobs expects 1..1024");
            } else {
                gen.chunk_instrs = number(value, INT_MAX);
                if (!gen.chunk_instrs) return snx_error("--chunk expects a positive instruction count");
            }
        } else if (!input)
            input = arg;
        else if (!name)
            name = arg;
        else
            return snx_error("unexpected argument: %s", arg);
    }
    if (!input) {
        usage();
        return 2;
    }
    if (!strcmp(target, "win64"))
        compile.target_win = 1;
    else if (!strcmp(target, "linux"))
        compile.target_win = 0;
    else if (strcmp(target, "auto"))
        return snx_error("--target expects auto, linux or win64");
    if (!no_build && host_win && !compile.target_win && (!runtime || !compile.cc))
        return snx_error("Linux cross-compilation on Windows requires --cc and --runtime");
    gen.abi = snx_read_abi(paths);
    if (!gen.abi) return snx_error("cannot read src/common/cpu/aarch64/cpu.h; set --repo to the checkout");
    if (snx_load_program(input, &program, error, sizeof(error))) return snx_error("%s", error);
    if (!program.npdm.is64) {
        snx_error("AArch32 programs are unsupported");
        goto done;
    }
    title = title_name(name, &program, program.entry_path);
    out = outdir ? snx_absolute(outdir) : snx_path_join(paths->repo, "games");
    if (!out) {
        snx_error("cannot resolve output directory");
        goto done;
    }
    if ((program.romfs_path && snx_path_within(out, program.romfs_path)) ||
        (program.exefs_path && snx_path_within(out, program.exefs_path))) {
        snx_error("output directory must be outside the input ExeFS/RomFS");
        goto done;
    }
    if (snx_mkdirs(out)) {
        snx_error("cannot create output directory");
        goto done;
    }
    base = snx_path_join(out, title);
    /* Invalidate the completion stamp before touching an existing game's data
       or module. Failed recompilation must never leave a trusted checkpoint. */
    if (!no_build) {
        char* stamp = snx_format("%s.abi", base);
        int failed = snx_is_file(stamp) && snx_remove_tree(stamp);
        free(stamp);
        if (failed) goto done;
    }
    data = snx_format("%s_data", base);
    if (snx_is_link(data)) {
        snx_error("invalid output directory");
        goto done;
    }
    /* Clang needs named files, all owned by this invocation's private temp directory. */
    build = snx_temp_dir(NULL);
    if (!build) {
        snx_error("cannot create temporary build directory");
        goto done;
    }
    gen.outdir = build;
    gen.title = title;
    gen.hints = read_hints(base, &gen.hint_count);
    if (!compile.quiet) printf("Recompiling %s (%lu modules)\n", title, (unsigned long)program.module_count);
    if (snx_generate_module(&program, &gen, &sources)) goto done;
    if ((keep_c || no_build) && save_sources(&sources, base)) goto done;
    if (copy_data(&program, data, compile.quiet)) {
        snx_error("cannot stage game data in %s", data);
        goto done;
    }
    ini = snx_format("%s.ini", base);
    if (!snx_is_file(ini)) {
        ini_text = snx_format(
            "; ScarletNX settings for %s\n; Full option help is written on first run.\n[Display]\nlanguage=1\n", title);
        if (snx_write_file(ini, ini_text, strlen(ini_text))) {
            snx_error("cannot write %s", ini);
            goto done;
        }
    }
    if (no_build) {
        rc = 0;
        goto done;
    }
    code = snx_format("%s_code.%s", base, compile.target_win ? "dll" : "so");
    if (snx_compile_sources(sources.paths, sources.count, code, paths, &compile)) goto done;
    if (runtime) run = snx_absolute(runtime);
#ifdef SNX_WITH_RUNTIME
    else if (host_win == compile.target_win)
        run = snx_strdup(paths->executable);
#endif
    else
        run = snx_build_runtime(paths, compile.target_win, compile.quiet);
    if (!run || !snx_is_file(run)) {
        snx_error("runtime unavailable; specify --runtime FILE");
        goto done;
    }
    /* Deploy the executable last: automatic play uses it as the completion checkpoint. */
    romfs = snx_path_join(data, "romfs");
    snx_prebuild_shaders(paths, romfs, out, compile.quiet);
    exe = snx_format("%s%s", base, compile.target_win ? ".exe" : "");
    tmp = snx_format("%s.tmp", exe);
    if (snx_copy_file(run, tmp) || snx_make_executable(tmp) || snx_replace(tmp, exe)) {
        snx_error("cannot deploy runtime to %s", exe);
        if (snx_is_file(tmp)) snx_remove_tree(tmp);
        goto done;
    }
    {
        char* stamp = snx_format("%s.abi", base);
        char version[32];
        sprintf(version, "%u\n", gen.abi);
        i = snx_write_file(stamp, version, strlen(version));
        free(stamp);
        if (i) {
            snx_error("cannot record the compiled game's ABI");
            goto done;
        }
    }
    if (!compile.quiet) printf("Built %s\n", exe);
    rc = 0;
done:
    if (build && snx_remove_tree(build)) {
        snx_error("cannot remove temporary build directory: %s", build);
        rc = 1;
    }
    snx_sources_free(&sources);
    snx_free_program(&program);
    free((void*)gen.hints);
    free(title);
    free(out);
    free(base);
    free(build);
    free(data);
    free(code);
    free(ini);
    free(ini_text);
    free(run);
    free(exe);
    free(tmp);
    free(romfs);
    return rc;
}
