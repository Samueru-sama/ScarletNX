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

/* The C toolchain of the portable backend is gone; this file builds the runtime for deployment. */
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
