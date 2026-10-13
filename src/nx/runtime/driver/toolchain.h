/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_TOOLCHAIN_H
#define SNX_TOOLCHAIN_H
#include "common.h"
typedef struct {
    const char* cc;
    const char* opt;
    unsigned jobs;
    int target_win, native, quiet;
} SnxCompileOptions;
int snx_compile_sources(char** sources, size_t count, const char* output, const SnxPaths* paths,
                        const SnxCompileOptions* options);
char* snx_build_runtime(const SnxPaths* paths, int target_win, int quiet);
int snx_prebuild_shaders(const SnxPaths* paths, const char* romfs, const char* outdir, int quiet);
#endif
