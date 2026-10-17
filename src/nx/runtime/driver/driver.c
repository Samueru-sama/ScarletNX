/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/runtime/driver/driver.h"
#include "common/utilities/common.h"
#include "nx/runtime/driver/extract.h"
#include "nx/runtime/driver/game_name.h"
#include "nx/runtime/driver/recompile.h"
#include "nx/runtime/cli.h"
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
         "  INPUT                  Extract, recompile if needed, then play XCI/NSP/DXCI/DNSP/NRO\n"
         "  -x, --extract          Extract XCI/NSP/DXCI/DNSP\n"
         "  -r, --recomp           Recompile ExeFS/NRO or prepare a dump without playing\n"
         "  -H, --headless         Run without a window or swapchain");
    puts("  -f, --fullscreen       Start fullscreen for this run (--fullscreen=0: windowed)\n"
         "                         Without either, the game's .ini decides\n"
         "  --touch=0|1|2          The game's touch screen: off, fingers, or fingers and the mouse\n"
         "  --controls=0|1         The on-screen controls: hidden or shown (the game's menu sets them)\n"
         "  --KEY=VALUE            Any setting of the game's .ini, for this run (--settings lists them)");
    puts("  --backend=llvm|gcc     Select the recompiler backend for preparation\n"
         "  -x -h / -r -h          Extraction / recompilation options\n"
         "  --game=games/Game      Run a compiled game");
    puts("  --outdir DIR           Played or prepared, where the game's folder goes (default: games/)\n"
         "  --out DIR              Played or prepared, the dump's extraction (default: work/<its name>)\n"
         "  --repo DIR             The data folder: games/, work/ and the keys (default: a\n"
         "                         .scarletnx folder beside the AppImage or the program,\n"
         "                         else the user's data folder,\n"
#ifdef _WIN32
         "                         %APPDATA%\\scarletnx)");
#elif defined(__APPLE__)
         "                         ~/Library/Application Support/scarletnx)");
#else
         "                         ~/.local/share/scarletnx)");
#endif
    puts("  --build-dir DIR        Override this build's build directory");
#ifndef _WIN32
    puts("  -S                     Enable the native GDB stub on localhost:1234\n"
         "  -s                     Enable the stub and stop at startup\n"
         "Short options may be combined, for example -rx or -Hs.");
#endif
}
/* the dump's extraction in work/, named after its file in the alphabet of the recompiler's explicit output name: the
   title ID is only known once it is extracted */
static char* dump_name(const char* input) {
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
static int cached_abi(const char* base) {
    char* stamp = snx_format("%s.abi", base);
    FILE* f = snx_fopen(stamp, "r");
    unsigned abi = 0, expected = snx_game_abi();
    if (f) {
        char newline, trailing;
        /* A partial write or trailing garbage is not a completed build. */
        if (fscanf(f, "%u%c%c", &abi, &newline, &trailing) != 2 || newline != '\n') abi = 0;
        fclose(f);
    }
    free(stamp);
    return expected && abi == expected;
}
/* games/ and the extraction in work/ of the data folder, else the folder outdir names and the extraction out names */
static int play_dump(const char* input, const SnxPaths* paths, unsigned flags, const char* backend, const char* outdir,
                     const char* out, int settings, char** setting, int game_argc, char** game_argv) {
    char *dumped = dump_name(input), *dump = snx_absolute(input), *name = NULL;
    char* games = outdir ? snx_absolute(outdir) : snx_path_join(paths->repo, "games");
    char* extracted = out ? snx_absolute(out) : NULL;
    char* work = !out ? snx_work_dir(paths->repo) : extracted ? snx_dirname(extracted) : NULL;
    char *pending, *game = NULL, *folder = NULL, *base = NULL, *code = NULL, error[256];
    const char** command = (const char**)snx_alloc((size_t)(game_argc + settings + 16) * sizeof(*command));
    int rc = 1, nro = is_nro_path(input), n, j;
    SnxNpdm npdm;
    if (!out && work) extracted = snx_path_join(work, dumped);
    pending = extracted ? snx_format("%s.extracting", extracted) : NULL;
    if (!pending || !games) {
        snx_error("cannot resolve the folders of %s", input);
        goto done;
    }
    if (nro && out) {
        snx_error("--out names a dump's extraction, and an NRO has none");
        goto done;
    }
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
    /* named as the recompile names it: by the title ID, which the extraction's NPDM gives */
    if (snx_load_npdm(nro ? dump : extracted, &npdm, error, sizeof error)) {
        rc = snx_error("%s", error);
        goto done;
    }
    name = snx_game_name(NULL, &npdm, nro ? dump : extracted);
    folder = snx_path_join(games, name); /* the game's own, as the recompile makes it */
    base = snx_path_join(folder, name);
#ifdef _WIN32
    code = snx_format("%s_code.dll", base);
    game = snx_format("%s.exe", base);
#else
    code = snx_format("%s_code.so", base);
    game = snx_strdup(base);
#endif
    /* Always restage an NRO: a same-named cached game may belong to another input or an older build. */
    /* An explicit backend must not silently reuse a game built by another compiler. */
    if (backend || nro || (flags & SNX_RECOMP) || !snx_is_file(game) || !snx_is_file(code) || !cached_abi(base)) {
        command[5] = "-r";
        command[6] = "--outdir";
        command[7] = games;
        n = 8;
        if (backend) {
            command[n++] = "--backend";
            command[n++] = backend;
        }
        command[n++] = "--";
        command[n++] = nro ? dump : extracted;
        command[n] = NULL;
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
#ifdef __ANDROID__
    /* an app's child process has no window to play in: the app runs the game this names (docs/ANDROID.md) */
    printf("Ready %s\n", name);
    rc = 0;
    goto done;
#endif
    n = 0;
    command[n++] = game;
    if (flags & SNX_HEADLESS) command[n++] = "-H";
    if (flags & SNX_FULLSCREEN) command[n++] = "-f";
    if (flags & SNX_WINDOWED) command[n++] = "--fullscreen=0";
    for (j = 0; j < settings; ++j) command[n++] = setting[j];
    if (flags & SNX_STOP)
        command[n++] = "-s";
    else if (flags & SNX_GDB)
        command[n++] = "-S";
    /* Keep the boundary so the deployed launcher cannot interpret game
       arguments as another extraction/recompilation request. */
    if (game_argc) command[n++] = "--";
    for (j = 0; j < game_argc; ++j) command[n++] = game_argv[j];
    command[n] = NULL;
    rc = (flags & SNX_GDB) ? snx_run_debug_stage(command, folder, "Game") : snx_run_stage(command, folder, "Game");
done:
    free(dumped);
    free(name);
    free(dump);
    free(work);
    free(games);
    free(extracted);
    free(pending);
    free(game);
    free(folder);
    free(base);
    free(code);
    free(command);
    return rc;
}
/* Options whose following token belongs to a tool, not to the launcher. */
static int takes_value(const char* arg) {
    static const char* const names[] = {"--update", "--name",    "--out",    "--keys",    "--title-keys",
                                        "-o",       "--outname", "--outdir", "--runtime", "--target",
                                        "-O",       "--jobs",    "--chunk",  "--backend"};
    size_t i;
    for (i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
        if (!strcmp(arg, names[i])) return 1;
    return 0;
}
/* key=value's key, a setting the runtime knows */
static int setting_known(const char* option) {
    char key[64];
    size_t n = (size_t)(strchr(option, '=') - option);
    if (!snx_setting_known || n >= sizeof key) return 0;
    memcpy(key, option, n);
    key[n] = 0;
    return snx_setting_known(key);
}
static int container_path(const char* path) {
    const char* ext = strrchr(path, '.');
    char lower[6];
    size_t i, size;
    size = ext ? strlen(ext) : 0;
    if (size != 4 && size != 5) return 0;
    for (i = 0; i <= size; ++i) lower[i] = (char)tolower((unsigned char)ext[i]);
    return !strcmp(lower, ".xci") || !strcmp(lower, ".nsp") || !strcmp(lower, ".dxci") || !strcmp(lower, ".dnsp");
}
int snx_driver(int argc, char** argv) {
    SnxPaths paths;
    const char *repo = NULL, *build = SNX_BUILD_DIR, *input = NULL, *backend = NULL, *outdir = NULL, *out = NULL;
    char *r = NULL, *b = NULL, *exe = NULL, **rest, **setting, *self, *data = NULL, *source = NULL;
    unsigned flags = 0;
    int i, n = 0, separator = argc, result = -1, tool_paths = 0, help = 0, parsed, automatic, backend_args = 0;
    int repo_given = 0, settings = 0;
    if (argc <= 1) return -1;
    rest = (char**)snx_alloc((size_t)(2 * argc + 1) * sizeof(*rest));
    setting = rest + argc + 1;
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
            if (is_repo) {
                repo = argv[i];
                repo_given = 1;
            } else
                build = argv[i];
            continue;
        }
        parsed = snx_cli_flag(argv[i], &flags);
        if (parsed < 0) {
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
        if (!strcmp(argv[i], "--backend") || !strncmp(argv[i], "--backend=", 10)) {
            const char* option = argv[i];
            rest[n++] = argv[i];
            ++backend_args;
            if (!strcmp(option, "--backend")) {
                if (++i == argc) {
                    result = snx_error("--backend requires llvm or gcc");
                    goto done;
                }
                backend = argv[i];
                rest[n++] = argv[i];
                ++backend_args;
            } else {
                backend = option + 10;
            }
            if (strcmp(backend, "llvm") && strcmp(backend, "gcc")) {
                result = snx_error("unknown backend: %s", backend);
                goto done;
            }
            continue;
        }
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
    /* played, a --key=value that is not the driver's own is a setting of the game's .ini, passed on to it when the
       runtime knows the key; any other is refused */
    for (i = 0; automatic && i < n; ++i)
        if (!strncmp(rest[i], "--", 2) && strchr(rest[i], '=') && strncmp(rest[i], "--backend=", 10)) {
            if (!setting_known(rest[i] + 2)) {
                result = snx_error("unknown option: %s", rest[i]);
                goto done;
            }
            setting[settings++] = rest[i];
        } else
            rest[i - settings] = rest[i];
    n -= settings;
    /* played, --outdir names the games' folder and --out the dump's extraction, as -r and -x take them */
    for (i = 0; automatic && i < n; ++i)
        if (!strcmp(rest[i], "--outdir") || !strcmp(rest[i], "--out")) {
            if (i + 1 == n) {
                result = snx_error("%s requires a directory", rest[i]);
                goto done;
            }
            *(strcmp(rest[i], "--out") ? &outdir : &out) = rest[i + 1];
            memmove(rest + i, rest + i + 2, (size_t)(n - i - 2) * sizeof(*rest));
            n -= 2;
            --i;
        }
    if (automatic && (n != 1 + backend_args || help)) {
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
        result = snx_error("-rx requires an XCI, NSP, DXCI or DNSP dump");
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
    /* work/, games/ and prod.keys are in the data folder (snx_data_dir), unless --repo names one (the menu does), for
       a build tree's driver as for an installed one. Not the current folder: a desktop entry or Explorer start the
       driver wherever they happen to be, often somewhere read-only (/, the AppImage's mount, Program Files) */
    self = snx_executable(argv[0]);
    exe = self ? snx_dirname(self) : NULL;
    free(self);
    source = snx_checkout(SNX_SOURCE_DIR, build, exe);
    if (!repo_given) {
        data = snx_data_dir(exe);
        if (data && snx_mkdirs(data)) {
            result = snx_error("cannot create the data folder %s", data);
            goto done;
        }
        repo = data ? data : ".";
    }
    free(exe);
    exe = NULL;
    snx_tool_console();
    if (data && source) snx_checkout_left(source, data);
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
    paths.source = source;
    if (automatic)
        result =
            play_dump(input, &paths, flags, backend, outdir, out, settings, setting,
                      separator < argc ? argc - separator - 1 : 0, argv + (separator < argc ? separator + 1 : argc));
    else
        result = (flags & SNX_RECOMP) ? snx_recompile(n, rest, &paths) : snx_extract(n, rest, &paths);
done:
    free(rest);
    free(r);
    free(b);
    free(exe);
    free(data);
    free(source);
    return result;
}
