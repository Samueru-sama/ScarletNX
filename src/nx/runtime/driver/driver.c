/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "driver.h"
#include "common.h"
#include "extract.h"
#include "recompile.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#ifndef SNX_SOURCE_DIR
#define SNX_SOURCE_DIR "."
#endif
#ifndef SNX_BUILD_DIR
#define SNX_BUILD_DIR "build"
#endif

static void usage(void) {
    puts("Usage: scarletnx [--repo DIR] [--build-dir DIR] COMMAND [OPTIONS]\n"
         "  INPUT                                     Play XCI/NSP or recompile and play NRO\n"
         "  --extract DUMP [--update DUMP] [--name NAME]  Extract XCI/NSP\n"
         "  --recompile INPUT [NAME] [OPTIONS]          Recompile ExeFS/NRO\n"
         "  --extract --help                           Extraction options\n"
         "  --recompile --help                         Recompiler options\n"
         "  --game=games/Game                          Run a compiled game");
    puts("  --headless[=0|1]                           Run without a window or swapchain\n"
         "Tool paths default to this build's checkout and build directory.");
}
/* Use the same filename alphabet as the recompiler's explicit output name. */
static char* game_name(const char* input) {
    const char *base = input, *p, *end;
    char* name;
    size_t n = 0;
    for (p = input; *p; ++p)
        if (*p == '/' || *p == '\\') base = p + 1;
    end = strrchr(base, '.');
    if (!end || end == base) end = base + strlen(base);
    name = (char*)snx_alloc((size_t)(end - base) + 1);
    for (p = base; p < end; ++p)
        if ((unsigned char)*p >= 128 || isalnum((unsigned char)*p) || *p == '-' || *p == '_' || *p == ' ')
            name[n++] = *p;
    while (n && name[n - 1] == ' ') --n;
    name[n] = 0;
    if (!n) {
        free(name);
        return snx_strdup("Game");
    }
    return name;
}
static int is_nro_path(const char* path) {
    const char* ext = strrchr(path, '.');
    return ext && strlen(ext) == 4 && tolower((unsigned char)ext[1]) == 'n' && tolower((unsigned char)ext[2]) == 'r' &&
           tolower((unsigned char)ext[3]) == 'o';
}
static int play_dump(const char* input, const SnxPaths* paths, const char* headless) {
    char *name = game_name(input), *dump = snx_absolute(input);
    char *work = snx_path_join(paths->repo, "work"), *games = snx_path_join(paths->repo, "games");
    char *extracted = snx_path_join(work, name), *pending = snx_format("%s.extracting", extracted);
    char* game;
    const char* command[12];
    int rc = 1, nro = is_nro_path(input);
#ifdef _WIN32
    game = snx_format("%s/%s.exe", games, name);
#else
    game = snx_path_join(games, name);
#endif
    if (!dump || !snx_is_file(dump)) {
        snx_error("Input dump is missing or is not a file: %s", input);
        goto done;
    }
    command[0] = paths->executable;
    command[1] = "--repo";
    command[2] = paths->repo;
    command[3] = "--build-dir";
    command[4] = paths->build;
    /* NROs are already executable images. No keys, hactool or extraction checkpoint is needed. */
    if (!nro && !snx_is_dir(extracted)) {
        /* Publish only a finished extraction. A killed extractor may leave its staging directory. */
        if (snx_mkdirs(work) ||
            ((snx_is_dir(pending) || snx_is_file(pending) || snx_is_link(pending)) && snx_remove_tree(pending))) {
            perror("scarletnx: preparing extraction directory");
            goto done;
        }
        command[5] = "--extract";
        command[6] = "--out";
        command[7] = pending;
        command[8] = "--";
        command[9] = dump;
        command[10] = NULL;
        rc = snx_run_stage(command, NULL, "Extraction");
        if (rc) goto done;
        if (!snx_is_dir(pending)) {
            rc = snx_error("Extraction finished without creating %s", pending);
            goto done;
        }
        if (snx_replace(pending, extracted)) {
            perror("scarletnx: publishing extraction directory");
            rc = 1;
            goto done;
        }
    }
    /* Always restage an NRO: a same-named cached game may belong to another input or an older build. */
    if (nro || !snx_is_file(game)) {
        command[5] = "--recompile";
        command[6] = "--outdir";
        command[7] = games;
        command[8] = "--";
        command[9] = nro ? dump : extracted;
        command[10] = name;
        command[11] = NULL;
        rc = snx_run_stage(command, NULL, "Recompilation");
        if (rc) goto done;
        if (!snx_is_file(game)) {
            rc = snx_error("Recompilation finished without creating %s", game);
            goto done;
        }
    }
    command[0] = game;
    command[1] = headless;
    command[2] = NULL;
    /* The child changes directory, leaving the caller's cwd untouched. Never cache a game run. */
    rc = snx_run_stage(command, games, "Game");
done:
    free(name);
    free(dump);
    free(work);
    free(games);
    free(extracted);
    free(pending);
    free(game);
    return rc;
}
int snx_driver(int argc, char** argv) {
    SnxPaths paths;
    const char *repo = SNX_SOURCE_DIR, *build = SNX_BUILD_DIR, *headless = NULL;
    char *r, *b, *exe;
    int i = 1, mode = 0, result, tool_paths = 0;
    if (argc <= 1) return -1;
    if (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h")) {
        snx_tool_console();
        usage();
        return 0;
    }
    while (i < argc) {
        if (!strcmp(argv[i], "--repo") || !strcmp(argv[i], "--build-dir")) {
            int is_repo = !strcmp(argv[i], "--repo");
            tool_paths = 1;
            if (++i == argc) return snx_error("%s requires a directory", argv[i - 1]);
            if (is_repo)
                repo = argv[i];
            else
                build = argv[i];
            ++i;
        } else if (!strcmp(argv[i], "--headless") || !strcmp(argv[i], "--headless=0") ||
                   !strcmp(argv[i], "--headless=1"))
            headless = argv[i++];
        else
            break;
    }
    if (i < argc && (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h"))) {
        snx_tool_console();
        usage();
        return 0;
    }
    if (i < argc && !strcmp(argv[i], "--extract")) mode = 1;
    if (i < argc && !strcmp(argv[i], "--recompile")) mode = 2;
    if (mode && headless) return snx_error("--headless applies to game execution, not extraction or recompilation");
    if (!mode && i < argc && argv[i][0] != '-') {
        int j;
        for (j = i + 1; j < argc; ++j) {
            if (!strcmp(argv[j], "--headless") || !strcmp(argv[j], "--headless=0") || !strcmp(argv[j], "--headless=1"))
                headless = argv[j];
            else
                return snx_error("automatic play accepts one XCI/NSP/NRO file and --headless[=0|1]");
        }
        mode = 3;
    }
    if (!mode) {
        if (tool_paths) return snx_error("expected an XCI/NSP/NRO file, --extract or --recompile");
        return -1;
    }
    snx_tool_console();
    r = snx_absolute(repo);
    b = snx_absolute(build);
    exe = snx_executable(argv[0]);
    if (!r || !b || !exe) {
        free(r);
        free(b);
        free(exe);
        return snx_error("cannot resolve tool paths");
    }
    paths.repo = r;
    paths.build = b;
    paths.executable = exe;
    if (mode == 3)
        result = play_dump(argv[i], &paths, headless);
    else
        result = mode == 1 ? snx_extract(argc - i - 1, argv + i + 1, &paths)
                           : snx_recompile(argc - i - 1, argv + i + 1, &paths);
    free(r);
    free(b);
    free(exe);
    return result;
}
