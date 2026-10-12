/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright 2026 the ScarletNX contributors
 * cc -std=c90 -pedantic scripts/recomp.c scripts/portable.c -o snx-recomp
 * Runtime build -> Python AOT -> NVN shader prebuild.
 * MinGW/llvm-mingw: add -lshell32. CMake: scripts/CMakeLists.txt.
 * Requires the scarletnx Python package. Usage: snx-recomp --help
 */
#include "portable.h"
#include <stdlib.h>
#include <string.h>

static void usage(void) {
    puts("Usage: snx-recomp GAME NAME [-- RECOMPILER_OPTIONS...]\n\n"
         "GAME/{exefs,romfs} -> games/:\n"
         "  NAME[.exe]           runtime (CMake preset: default)\n"
         "  NAME_code.so/.dll    AOT game code\n"
         "  NAME_data/           exefs, romfs + saves\n"
         "  shader_cache/spirv/  NVN shaders, if present\n");
    puts("After --: python -m scarletnx options; --cc defaults to clang.\n"
         "SNX_PYTHON: Python executable (default: " SNX_PYTHON ").\n"
         "SNX_REPO: checkout override.");
}

static void shader_walk(const char* folder, const char* root, const char* shaderc, const char* cache) {
    static const unsigned char magic[4] = {0x91, 0x68, 0x86, 0x19};
    Strings files;
    char* path;
    FILE* file;
    unsigned char header[4];
    size_t i, count;
    int found;
    char* args[6];
    Process process;
    files = list_dir(folder, 0); /* os.walk ignores directory enumeration errors. */
    found = 0;
    for (i = 0; i < files.count && !found; ++i) {
        path = join(folder, files.items[i]);
        if (!is_dir(path)) {
            file = open_file(path, "rb");
            count = fread(header, 1, sizeof(header), file);
            close_file(file, path);
            found = count == sizeof(header) && !memcmp(header, magic, sizeof(header));
        }
        release(path);
    }
    if (found) {
        make_dirs(cache);
        printf("Shaders: %s\n", !strcmp(folder, root) ? "." : folder + strlen(root) + 1);
        fflush(stdout);
        args[0] = (char*)shaderc;
        args[1] = "--nvn";
        args[2] = (char*)folder;
        args[3] = "--cache";
        args[4] = (char*)cache;
        args[5] = NULL;
        process = run_process(args, NULL, 0, 0, NULL);
        if (process.status) {
            printf("snx-shaderc: exit %d (nonfatal); rerun with --verbose\n", process.status);
            fflush(stdout);
        }
        process_free(&process);
    }
    for (i = 0; i < files.count; ++i) {
        path = join(folder, files.items[i]);
        /* Match os.walk(followlinks=False), including Windows junctions. */
        if (is_dir(path) && !is_link(path)) shader_walk(path, root, shaderc, cache);
        release(path);
    }
    strings_free(&files);
}

int main(int argc, char** argv) {
    const char* game;
    const char* name;
    const char* python;
    char* repo;
    char* game_absolute;
    char* runtime;
    char* games;
    char* shaderc;
    char* data_name;
    char* data_dir;
    char* romfs;
    char* cache;
    char* configure[4];
    char* build[5];
    Strings extra, command;
    Process process;
    int i, positional, have_cc;
    size_t n;

    host_arguments(&argc, &argv);
    game = NULL;
    name = NULL;
    positional = 0;
    memset(&extra, 0, sizeof(extra));
    memset(&command, 0, sizeof(command));
    for (i = 1; i < argc; ++i) {
        if (!positional && (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h"))) {
            usage();
            return 0;
        }
        if (!positional && !strcmp(argv[i], "--")) {
            positional = 1;
            continue;
        }
        if (!positional && argv[i][0] == '-' && argv[i][1]) {
            fprintf(stderr, "unknown option: %s; recompiler flags follow --\n", argv[i]);
            return 2;
        }
        if (!game)
            game = argv[i];
        else if (!name)
            name = argv[i];
        else
            strings_add(&extra, argv[i]);
    }
    if (!game || !name) {
        usage();
        return 2;
    }
    repo = repo_path(argv[0]);
    /* Resolve against caller cwd, not the child checkout cwd. */
    game_absolute = absolute_path(game);
    configure[0] = "cmake";
    configure[1] = "--preset";
    configure[2] = "default";
    configure[3] = NULL;
    require_process(configure, repo, 1);
    build[0] = "cmake";
    build[1] = "--build";
    build[2] = "--preset";
    build[3] = "default";
    build[4] = NULL;
    require_process(build, repo, 0);
    runtime = join(repo, "build/default/scarletnx" SNX_EXE);
    games = join(repo, "games");
    python = environment("SNX_PYTHON");
    if (!python || !*python) python = SNX_PYTHON;
    strings_add(&command, python);
    strings_add(&command, "-m");
    strings_add(&command, "scarletnx");
    strings_add(&command, game_absolute);
    strings_add(&command, "-o");
    strings_add(&command, name);
    strings_add(&command, "--outdir");
    strings_add(&command, games);
    strings_add(&command, "--target");
    strings_add(&command, SNX_TARGET);
    strings_add(&command, "--runtime");
    strings_add(&command, runtime);
    have_cc = 0;
    for (n = 0; n < extra.count; ++n) {
        strings_add(&command, extra.items[n]);
        if (!strcmp(extra.items[n], "--cc") || !strncmp(extra.items[n], "--cc=", 5)) have_cc = 1;
    }
    if (!have_cc) {
        strings_add(&command, "--cc");
        strings_add(&command, "clang");
    }
    process = run_process(command.items, repo, 0, 0, NULL);
    if (!process.status) {
        shaderc = join(repo, "build/default/snx-shaderc" SNX_EXE);
        data_name = concat(name, "_data");
        data_dir = join(games, data_name);
        romfs = join(data_dir, "romfs");
        if (is_file(shaderc) && is_dir(romfs)) {
            cache = join(games, "shader_cache/spirv");
            shader_walk(romfs, romfs, shaderc, cache);
        }
    }
    strings_free(&extra);
    strings_free(&command);
    process_free(&process);
    return process.status;
}
