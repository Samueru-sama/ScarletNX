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
    char *on[] = {"game", "--headless"}, *short_on[] = {"game", "-H"};
    char *explicit_on[] = {"game", "--headless=1"}, *off[] = {"game", "--headless=0"};
    char *bad[] = {"game", "--headless=yes"}, *empty[] = {"game", "--headless="};
    char *other[] = {"game", "--seconds=1"}, *boundary[] = {"game", "--", "-Hs", "--headless=0"};
    char *combined[] = {"game", "-Hs"}, *server[] = {"game", "-S"};
    if (check(2, on, 0, 1) || check(2, short_on, 0, 1) || check(2, explicit_on, 2, 0) || check(2, off, 2, 0) ||
        check(2, bad, 2, 0) || check(2, empty, 2, 0) || check(2, other, 0, 0) || check(4, boundary, 0, 0))
        return 1;
    if (g_opt_gdb || g_opt_gdb_stop) return 1;
#ifdef _WIN32
    if (check(2, combined, 2, 0) || check(2, server, 2, 0)) return 1;
#else
    if (check(2, combined, 0, 1) || !g_opt_gdb || !g_opt_gdb_stop) return 1;
    if (check(2, server, 0, 0) || !g_opt_gdb || g_opt_gdb_stop) return 1;
#endif
    {
        char *fullscreen[] = {"game", "--fullscreen"}, *short_fullscreen[] = {"game", "-f"};
        char *fullscreen_off[] = {"game", "--fullscreen=0"}, *fullscreen_on[] = {"game", "--fullscreen=1"};
        char *fullscreen_boundary[] = {"game", "--", "-f"};
        if (check(2, fullscreen, 0, 0) || !g_opt_fullscreen || check(2, short_fullscreen, 0, 0) ||
            !g_opt_fullscreen || check(3, fullscreen_boundary, 0, 0) || g_opt_fullscreen ||
            check(1, fullscreen, 0, 0) || g_opt_fullscreen || check(2, fullscreen_off, 2, 0) ||
            check(2, fullscreen_on, 2, 0)) return 1;
#ifndef _WIN32
        {
            char* cluster[] = {"game", "-fHs"};
            if (check(2, cluster, 0, 1) || !g_opt_fullscreen || !g_opt_gdb || !g_opt_gdb_stop) return 1;
        }
#endif
    }
    puts("Startup options passed");
    return 0;
}
