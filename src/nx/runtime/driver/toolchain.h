/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_TOOLCHAIN_H
#define SNX_TOOLCHAIN_H
#include "common.h"
typedef struct {
    const char* opt;
    unsigned jobs;
    int target_win, quiet;
} SnxCompileOptions;
char* snx_build_runtime(const SnxPaths* paths, int target_win, int quiet);
#endif
