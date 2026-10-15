/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "browser.h"
#include "common.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    GuiGames games = {0};
    char error[256];
    char* dir = snx_temp_dir(NULL);
    const char* files[] = {"Alpha.CIA",    "game with spaces.cia", ".hidden.cia",
                           "wrong.cia.so", "launch.exe",           "note.txt"};
    assert(dir);
    for (size_t i = 0; i < sizeof files / sizeof *files; ++i) {
        char* path = snx_path_join(dir, files[i]);
        assert(!snx_write_file(path, "fixture", 7));
        free(path);
    }
    char* subdir = snx_path_join(dir, "directory.cia");
    assert(!snx_mkdirs(subdir));
    assert(gui_cias_scan(&games, dir, error, sizeof error) && !error[0]);
    assert(games.count == 2 && !strcmp(games.names[0], "Alpha.CIA") && !strcmp(games.names[1], "game with spaces.cia"));
    /* Repeated scans replace the old list; neither recursion nor execute bits. */
    assert(gui_cias_scan(&games, subdir, error, sizeof error) && !games.count);
    assert(!snx_remove_tree(dir));
    assert(!gui_cias_scan(&games, dir, error, sizeof error) && error[0] && !games.count);
    gui_games_free(&games);
    free(subdir);
    free(dir);
    puts("GUI CIA browser filtering, sorting and refresh passed");
    return 0;
}
