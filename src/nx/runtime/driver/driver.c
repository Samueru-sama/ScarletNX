/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "driver.h"
#include "common.h"
#include "extract.h"
#include "recompile.h"
#include "../cli.h"
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
    puts("Usage: scarletnx [OPTIONS] INPUT [-- GAME_ARGUMENTS...]\n"
         "  INPUT                  Extract, recompile if needed, then play XCI/NSP/NRO\n"
         "  -x, --extract          Extract XCI/NSP\n"
         "  -r, --recomp           Recompile ExeFS/NRO or prepare a dump without playing\n"
         "  -H, --headless         Run without a window or swapchain");
    puts("  --fullscreen[=0|1]     Override the game's fullscreen setting\n"
         "  -x -h / -r -h          Extraction / recompilation options\n"
         "  --game=games/Game      Run a compiled game\n"
         "  --repo / --build-dir   Override this build's checkout / build directory");
#ifndef _WIN32
    puts("  -S                     Enable the native GDB stub on localhost:1234\n"
         "  -s                     Enable the stub and stop at startup\n"
         "Short options may be combined, for example -rx or -Hs.");
#endif
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
static int cached_abi(const char* base, const SnxPaths* paths) {
    char* stamp = snx_format("%s.abi", base);
    FILE* f = snx_fopen(stamp, "r");
    unsigned abi = 0, expected = snx_read_abi(paths);
    if (f) {
        char newline, trailing;
        /* A partial write or trailing garbage is not a completed build. */
        if (fscanf(f, "%u%c%c", &abi, &newline, &trailing) != 2 || newline != '\n') abi = 0;
        fclose(f);
    }
    free(stamp);
    return expected && abi == expected;
}
static int play_dump(const char* input, const SnxPaths* paths, unsigned flags, int game_argc, char** game_argv) {
    char *name = game_name(input), *dump = snx_absolute(input);
    char *work = snx_path_join(paths->repo, "work"), *games = snx_path_join(paths->repo, "games");
    char *extracted = snx_path_join(work, name), *pending = snx_format("%s.extracting", extracted);
    char *game, *base = snx_path_join(games, name), *code;
    const char** command = (const char**)snx_alloc((size_t)(game_argc + 16) * sizeof(*command));
    int rc = 1, nro = is_nro_path(input), n, j;
#ifdef _WIN32
    code = snx_format("%s_code.dll", base);
#else
    code = snx_format("%s_code.so", base);
#endif
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
        command[5] = "-x";
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
    if (nro || (flags & SNX_RECOMP) || !snx_is_file(game) || !snx_is_file(code) || !cached_abi(base, paths)) {
        command[5] = "-r";
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
    if (flags & SNX_RECOMP) {
        rc = 0;
        goto done;
    }
    n = 0;
    command[n++] = game;
    if (flags & SNX_HEADLESS) command[n++] = "-H";
    if (flags & SNX_FULLSCREEN)
        command[n++] = "--fullscreen";
    else if (flags & SNX_WINDOWED)
        command[n++] = "--fullscreen=0";
    if (flags & SNX_STOP)
        command[n++] = "-s";
    else if (flags & SNX_GDB)
        command[n++] = "-S";
    /* Keep the boundary so the deployed launcher cannot interpret game
       arguments as another extraction/recompilation request. */
    if (game_argc) command[n++] = "--";
    for (j = 0; j < game_argc; ++j) command[n++] = game_argv[j];
    command[n] = NULL;
    rc = (flags & SNX_GDB) ? snx_run_debug_stage(command, games, "Game") : snx_run_stage(command, games, "Game");
done:
    free(name);
    free(dump);
    free(work);
    free(games);
    free(extracted);
    free(pending);
    free(game);
    free(base);
    free(code);
    free(command);
    return rc;
}
/* Options whose following token belongs to a tool, not to the launcher. */
static int takes_value(const char* arg) {
    static const char* const names[] = {"--update", "--name",    "--out",    "--keys",    "--title-keys",
                                        "-o",       "--outname", "--outdir", "--runtime", "--target",
                                        "--cc",     "-O",        "--jobs",   "--chunk"};
    size_t i;
    for (i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
        if (!strcmp(arg, names[i])) return 1;
    return 0;
}
static int container_path(const char* path) {
    const char* ext = strrchr(path, '.');
    char lower[5];
    int i;
    if (!ext || strlen(ext) != 4) return 0;
    for (i = 0; i < 5; ++i) lower[i] = (char)tolower((unsigned char)ext[i]);
    return !strcmp(lower, ".xci") || !strcmp(lower, ".nsp");
}
int snx_driver(int argc, char** argv) {
    SnxPaths paths;
    const char *repo = SNX_SOURCE_DIR, *build = SNX_BUILD_DIR, *input = NULL;
    char *r = NULL, *b = NULL, *exe = NULL, **rest;
    unsigned flags = 0;
    int i, n = 0, separator = argc, result = -1, tool_paths = 0, help = 0, parsed, automatic;
    if (argc <= 1) return -1;
    rest = (char**)snx_alloc((size_t)(argc + 1) * sizeof(*rest));
    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--")) {
            separator = i;
            break;
        }
        if (!strcmp(argv[i], "--repo") || !strcmp(argv[i], "--build-dir")) {
            int is_repo = !strcmp(argv[i], "--repo");
            tool_paths = 1;
            if (++i == argc) {
                result = snx_error("%s requires a directory", argv[i - 1]);
                goto done;
            }
            if (is_repo)
                repo = argv[i];
            else
                build = argv[i];
            continue;
        }
        parsed = snx_cli_flag(argv[i], &flags);
        if (parsed < 0 || !strcmp(argv[i], "--recompile")) {
            result = snx_error("unknown option: %s", argv[i]);
            goto done;
        }
#ifdef _WIN32
        if (!strcmp(argv[i], "-s") || !strcmp(argv[i], "-S")) {
            result = snx_error("the GDB stub is UNIX only");
            goto done;
        }
#endif
        if (parsed) continue;
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) help = 1;
        if (!input && argv[i][0] != '-') input = argv[i];
        rest[n++] = argv[i];
        if (takes_value(argv[i]) && i + 1 < argc) rest[n++] = argv[++i];
    }
    if (help && !(flags & (SNX_EXTRACT | SNX_RECOMP))) {
        snx_tool_console();
        usage();
        result = 0;
        goto done;
    }
    automatic = input && (!(flags & (SNX_EXTRACT | SNX_RECOMP)) || ((flags & SNX_RECOMP) && container_path(input)));
    if (automatic && (n != 1 || help)) {
        result = snx_error("automatic preparation accepts one dump file. Pass game arguments after --");
        goto done;
    }
    if ((flags & (SNX_EXTRACT | SNX_RECOMP)) && (flags & (SNX_HEADLESS | SNX_GDB | SNX_FULLSCREEN | SNX_WINDOWED))) {
        result = snx_error("window and debugger options apply to game execution, not preparation");
        goto done;
    }
    if (!automatic && !(flags & (SNX_EXTRACT | SNX_RECOMP))) {
        if (tool_paths) result = snx_error("expected a dump, -x or -r");
        goto done;
    }
    if (!automatic && (flags & SNX_EXTRACT) && (flags & SNX_RECOMP)) {
        result = snx_error("-rx requires an XCI or NSP dump");
        goto done;
    }
    if (automatic && (flags & SNX_RECOMP) && separator < argc) {
        result = snx_error("game arguments require game execution");
        goto done;
    }
    /* Explicit tool modes retain their positional -- escape (including names
       beginning with '-'). Automatic play uses it for game arguments instead. */
    if (!automatic)
        for (i = separator; i < argc; ++i) rest[n++] = argv[i];
    rest[n] = NULL;
    snx_tool_console();
    r = snx_absolute(repo);
    b = snx_absolute(build);
    exe = snx_executable(argv[0]);
    if (!r || !b || !exe) {
        result = snx_error("cannot resolve tool paths");
        goto done;
    }
    paths.repo = r;
    paths.build = b;
    paths.executable = exe;
    if (automatic)
        result = play_dump(input, &paths, flags, separator < argc ? argc - separator - 1 : 0,
                           argv + (separator < argc ? separator + 1 : argc));
    else
        result = (flags & SNX_RECOMP) ? snx_recompile(n, rest, &paths) : snx_extract(n, rest, &paths);
done:
    free(rest);
    free(r);
    free(b);
    free(exe);
    return result;
}
