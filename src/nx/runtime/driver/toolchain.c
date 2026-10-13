/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later (LICENSE). */
#define _POSIX_C_SOURCE 200809L
#include "toolchain.h"
#include "module.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* The C toolchain of the portable backend (removed) is gone: what is left builds the runtime and prebuilds shaders */
static void diagnostic(const char* stage, const char* output) {
    size_t n = output ? strlen(output) : 0;
    snx_error("%s failed%s%s", stage, n ? ":\n" : "", n ? output + (n > 4000 ? n - 4000 : 0) : "");
}
char* snx_build_runtime(const SnxPaths* paths, int target_win, int quiet) {
    char *bdir, *runtime, *output = NULL;
    const char *preset = "default", *args[10];
    int rc;
#ifndef _WIN32
    if (target_win) preset = "windows-cross";
#endif
    bdir = snx_path_join(paths->repo, target_win ? "build/win64-recomp" : "build/native-recomp");
    if (!quiet) printf("Building runtime in %s\n", bdir);
    args[0] = "cmake";
    args[1] = "--preset";
    args[2] = preset;
    args[3] = "-B";
    args[4] = bdir;
    args[5] = "-DSNX_DEPLOY=OFF";
    args[6] = "-DSNX_TOOLS_ONLY=OFF";
    args[7] = NULL;
    rc = snx_run(args, paths->repo, NULL, &output);
    if (!rc) {
        free(output);
        output = NULL;
        args[1] = "--build";
        args[2] = bdir;
        args[3] = NULL;
        rc = snx_run(args, paths->repo, NULL, &output);
    }
    if (rc) {
        diagnostic("Runtime build", output);
        runtime = NULL;
    } else
        runtime = snx_path_join(bdir, target_win ? "scarletnx.exe" : "scarletnx");
    free(bdir);
    free(output);
    return runtime;
}
static int scan_shaders(const char* shaderc, const char* dir, const char* cache, int quiet) {
    char **names, *path, *output;
    const char* args[6];
    size_t n, i;
    unsigned char magic[4];
    int found = 0;
    FILE* f;
    if (snx_list_dir(dir, &names, &n)) return 1;
    for (i = 0; i < n; ++i) {
        path = snx_path_join(dir, names[i]);
        if (snx_is_link(path)) {
            free(path);
            continue;
        }
        if (snx_is_dir(path))
            scan_shaders(shaderc, path, cache, quiet);
        else if (!found && (f = snx_fopen(path, "rb")) != NULL) {
            if (fread(magic, 1, 4, f) == 4 && !memcmp(magic, "\x91\x68\x86\x19", 4)) found = 1;
            fclose(f);
        }
        free(path);
    }
    snx_free_strings(names, n);
    if (found) {
        if (snx_mkdirs(cache)) return 1;
        if (!quiet) printf("Prebuilding shaders: %s\n", dir);
        args[0] = shaderc;
        args[1] = "--nvn";
        args[2] = dir;
        args[3] = "--cache";
        args[4] = cache;
        args[5] = NULL;
        output = NULL;
        if (snx_run(args, NULL, NULL, &output))
            fputs("Shader prebuild failed; shaders will compile at runtime.\n", stderr);
        free(output);
    }
    return 0;
}
int snx_prebuild_shaders(const SnxPaths* paths, const char* romfs, const char* outdir, int quiet) {
    char *shaderc, *cache;
    int rc = 0;
#ifdef _WIN32
    shaderc = snx_path_join(paths->build, "snx-shaderc.exe");
#else
    shaderc = snx_path_join(paths->build, "snx-shaderc");
#endif
    cache = snx_path_join(outdir, "shader_cache/spirv");
    if (snx_is_file(shaderc) && snx_is_dir(romfs)) rc = scan_shaders(shaderc, romfs, cache, quiet);
    free(shaderc);
    free(cache);
    return rc;
}
