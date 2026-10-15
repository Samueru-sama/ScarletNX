/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "browser.h"
#include "common.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif

static bool suffix(const char* name, const char* extension) {
    size_t n = strlen(name), e = strlen(extension);
    if (n < e) return false;
    for (size_t i = 0; i < e; ++i)
        if (tolower((unsigned char)name[n - e + i]) != extension[i]) return false;
    return true;
}
void gui_games_free(GuiGames* games) {
    snx_free_strings(games->names, games->count);
    games->names = NULL;
    games->count = 0;
}
static bool scan(GuiGames* games, const char* directory, char* error, size_t size, bool cia) {
    gui_games_free(games);
    if (size) error[0] = 0;
    char** names = NULL;
    size_t count = 0;
    if (snx_list_dir(directory, &names, &count)) {
        snprintf(error, size, "Cannot open games/. Create it in the displayed working directory.");
        return false;
    }
    games->names = calloc(count ? count : 1, sizeof(*games->names));
    if (!games->names) {
        snx_free_strings(names, count);
        snprintf(error, size, "Not enough memory to list games/.");
        return false;
    }
    /* snx_list_dir already sorts names. Libraries often carry executable bits,
       but they are not game launchers. Do not show them or hidden files. */
    for (size_t i = 0; i < count; ++i) {
        const char* name = names[i];
        if (name[0] == '.') continue;
        if (cia) {
            if (!suffix(name, ".cia")) continue;
            char* path = snx_path_join(directory, name);
            bool regular = path && snx_is_file(path);
            free(path);
            if (regular) {
                games->names[games->count++] = names[i];
                names[i] = NULL;
            }
            continue;
        }
#ifdef _WIN32
        if (!suffix(name, ".exe")) continue;
#else
        if (suffix(name, ".so") || strstr(name, ".so.") || suffix(name, ".dll") || suffix(name, ".dylib")) continue;
#endif
        char* path = snx_path_join(directory, name);
#ifndef _WIN32
        if (access(path, X_OK)) {
            free(path);
            continue;
        }
#endif
        char* executable = snx_find_tool(path); /* regular file, with UTF-8 paths on Windows */
        free(path);
        if (!executable) continue;
        free(executable);
        games->names[games->count++] = names[i];
        names[i] = NULL;
    }
    snx_free_strings(names, count);
    return true;
}
bool gui_games_scan(GuiGames* games, const char* directory, char* error, size_t size) {
    return scan(games, directory, error, size, false);
}
bool gui_cias_scan(GuiGames* games, const char* directory, char* error, size_t size) {
    return scan(games, directory, error, size, true);
}
