/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Startup-only switches, parsed before loading game code or showing any fatal error box. */
#include "runtime/options.h"
#include <stdio.h>
#include <string.h>

#ifdef SNX_HEADLESS_ONLY
int g_opt_headless = 1;
#else
int g_opt_headless;
#endif

int snx_startup_options(int argc, char** argv) {
    int i, headless = g_opt_headless;
    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--headless") || !strcmp(argv[i], "--headless=1"))
            headless = 1;
        else if (!strcmp(argv[i], "--headless=0"))
            headless = 0;
        else if (!strncmp(argv[i], "--headless=", 11)) {
            fputs("scarletnx: --headless expects 0 or 1\n", stderr);
            return 2;
        }
    }
#ifdef SNX_HEADLESS_ONLY
    if (!headless) {
        fputs("scarletnx: --headless=0 requires a build with the SDL3 window backend\n", stderr);
        return 2;
    }
#endif
    g_opt_headless = headless;
    return 0;
}
