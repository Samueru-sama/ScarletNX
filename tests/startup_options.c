/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "runtime/options.h"
#include <stdio.h>

static int check(int argc, char** argv, int expected, int value) {
    int result = snx_startup_options(argc, argv);
    if (result == expected && (result || g_opt_headless == value)) return 0;
    fprintf(stderr, "startup options: got status %d, headless %d\n", result, g_opt_headless);
    return 1;
}
int main(void) {
    char *on[] = {"game", "--headless"}, *explicit_on[] = {"game", "--headless=1"};
    char *off[] = {"game", "--headless=0"}, *bad[] = {"game", "--headless=yes"};
    char *empty[] = {"game", "--headless="}, *last[] = {"game", "--headless=0", "--headless"};
    char* other[] = {"game", "--seconds=1"};
#ifdef SNX_HEADLESS_ONLY
    if (!g_opt_headless || check(2, off, 2, 0)) return 1;
#else
    if (g_opt_headless || check(2, off, 0, 0)) return 1;
#endif
    if (check(2, on, 0, 1) || check(2, explicit_on, 0, 1) || check(2, bad, 2, 0) || check(2, empty, 2, 0) ||
        check(3, last, 0, 1) || check(2, other, 0, 1))
        return 1;
#ifndef SNX_HEADLESS_ONLY
    if (check(2, off, 0, 0)) return 1;
#endif
    puts("Headless startup options passed");
    return 0;
}
