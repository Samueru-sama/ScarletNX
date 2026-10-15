/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Exercise the real driver/supervisor with synthetic extraction, compilation and game children. */
#define _POSIX_C_SOURCE 200809L
#include "common.h"
#include "driver.h"
#include "extract.h"
#include "recompile.h"
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif

static unsigned checks;
static void check(int good, const char* expression, int line) {
    ++checks;
    if (!good) {
        fprintf(stderr, "driver_workflow.c:%d: check failed: %s\n", line, expression);
        exit(90);
    }
}
#define CHECK(value) check((value) != 0, #value, __LINE__)
static void set_env(const char* name, const char* value) {
#ifdef _WIN32
    CHECK(!_putenv_s(name, value));
#else
    CHECK(!setenv(name, value, 1));
#endif
}
static int event(char stage) {
    FILE* log = snx_fopen(getenv("SNX_WORKFLOW_LOG"), "a");
    const char *fail = getenv("SNX_WORKFLOW_FAIL"), *mode = getenv("SNX_WORKFLOW_MODE");
    CHECK(log != NULL);
    CHECK(fputc(stage, log) != EOF);
    CHECK(!fclose(log));
    if (!fail || fail[0] != stage || !strcmp(mode, "missing")) return 0;
#ifndef _WIN32
    if (!strcmp(mode, "interrupt")) raise(SIGINT);
    if (!strcmp(mode, "terminate")) raise(SIGTERM);
    if (!strcmp(mode, "stop")) raise(SIGSTOP);
    if (!strcmp(mode, "parent")) {
        int ready[2];
        char byte;
        pid_t worker;
        CHECK(!pipe(ready));
        worker = fork();
        CHECK(worker >= 0);
        if (!worker) {
            close(ready[0]);
            signal(SIGTERM, SIG_IGN);
            CHECK(write(ready[1], "x", 1) == 1);
            close(ready[1]);
            sleep(2);
            /* A surviving compiler grandchild would keep the capture pipe open and log X. */
            event('X');
            _exit(0);
        }
        close(ready[1]);
        CHECK(read(ready[0], &byte, 1) == 1);
        close(ready[0]);
        CHECK(!kill(getppid(), SIGTERM));
        for (;;) pause();
    }
#else
    (void)mode;
#endif
    return 7;
}
int snx_extract(int argc, char** argv, const SnxPaths* paths) {
    char* partial;
    int rc;
    (void)paths;
    CHECK(argc == 4 && !strcmp(argv[0], "--out") && !strcmp(argv[2], "--"));
    CHECK(snx_is_file(argv[3]));
    CHECK(!snx_is_dir(argv[1]));
    CHECK(!snx_mkdirs(argv[1]));
    partial = snx_path_join(argv[1], "partial");
    CHECK(!snx_write_file(partial, "partial", 7));
    free(partial);
    rc = event('E');
    if (!strcmp(getenv("SNX_WORKFLOW_MODE"), "missing") && getenv("SNX_WORKFLOW_FAIL") &&
        !strcmp(getenv("SNX_WORKFLOW_FAIL"), "E"))
        CHECK(!snx_remove_tree(argv[1]));
    return rc;
}
unsigned snx_game_abi(void) { return 4; }
int snx_recompile(int argc, char** argv, const SnxPaths* paths) {
    char *game, *partial;
    int rc;
    if (getenv("SNX_WORKFLOW_BACKEND") && *getenv("SNX_WORKFLOW_BACKEND")) {
        CHECK(argc == 7 && !strcmp(argv[2], "--backend") && !strcmp(argv[3], getenv("SNX_WORKFLOW_BACKEND")));
        /* Normalize for the shared synthetic compilation assertions below. */
        argv[2] = argv[4];
        argv[3] = argv[5];
        argv[4] = argv[6];
        argc = 5;
    }
    CHECK(argc == 5 && !strcmp(argv[0], "--outdir") && !strcmp(argv[2], "--"));
    if (getenv("SNX_WORKFLOW_NRO") && *getenv("SNX_WORKFLOW_NRO")) {
        char *given, *expected;
        CHECK(snx_is_file(argv[3]));
        /* The driver hands the stage a resolved path (Windows: backslashes and long
           names), so compare canonical spellings like the game's cwd check does. */
        given = snx_absolute(argv[3]);
        expected = snx_absolute(getenv("SNX_WORKFLOW_NRO"));
        CHECK(given && expected && !strcmp(given, expected));
        free(given);
        free(expected);
    } else
        CHECK(snx_is_dir(argv[3]));
    CHECK(!snx_mkdirs(argv[1]));
    partial = snx_path_join(argv[1], "partial-build");
    CHECK(!snx_write_file(partial, "partial", 7));
    free(partial);
    rc = event('R');
    if (rc || (!strcmp(getenv("SNX_WORKFLOW_MODE"), "missing") && getenv("SNX_WORKFLOW_FAIL") &&
               !strcmp(getenv("SNX_WORKFLOW_FAIL"), "R")))
        return rc;
#ifdef _WIN32
    game = snx_format("%s/%s.exe", argv[1], argv[4]);
#else
    game = snx_path_join(argv[1], argv[4]);
#endif
    CHECK(!snx_copy_file(paths->executable, game));
    CHECK(!snx_make_executable(game));
    {
        char* stamp = snx_format("%s/%s.abi", argv[1], argv[4]);
        char* code = snx_format("%s/%s_code.%s", argv[1], argv[4],
#ifdef _WIN32
                                "dll"
#else
                                "so"
#endif
        );
        CHECK(!snx_write_file(stamp, "4\n", 2));
        CHECK(!snx_write_file(code, "synthetic", 9));
        free(stamp);
        free(code);
    }
    free(game);
    return 0;
}
static void run(const char* exe, const char* repo, const char* dump, const char* log, int expected, const char* events,
                const char* diagnostic) {
    const char* args[16];
    const char *headless = getenv("SNX_WORKFLOW_HEADLESS"), *flags = getenv("SNX_WORKFLOW_FLAGS");
    int n = 5;
    char* output = NULL;
    unsigned char* recorded;
    size_t size;
    int rc;
    CHECK(!snx_write_file(log, "", 0));
    args[0] = exe;
    args[1] = "--repo";
    args[2] = repo;
    args[3] = "--build-dir";
    args[4] = repo;
    if (flags && *flags && strncmp(flags, "--backend=", 10)) args[n++] = flags;
    if (headless && *headless && !strcmp(getenv("SNX_WORKFLOW_PREFIX"), "1")) args[n++] = headless;
    args[n++] = dump;
    if (flags && !strncmp(flags, "--backend=", 10)) args[n++] = flags;
    if (headless && *headless && strcmp(getenv("SNX_WORKFLOW_PREFIX"), "1")) args[n++] = headless;
    if (getenv("SNX_WORKFLOW_ARGS") && *getenv("SNX_WORKFLOW_ARGS")) {
        args[n++] = "--";
        args[n++] = "some arg";
        args[n++] = "";
        args[n++] = "-rx";
        args[n++] = "--headless=0";
    }
    args[n] = NULL;
    rc = snx_run(args, NULL, NULL, &output);
    if (rc != expected) fprintf(stderr, "expected %d, got %d: %s\n", expected, rc, output);
    CHECK(rc == expected);
    if (diagnostic) CHECK(strstr(output, diagnostic) != NULL);
    recorded = snx_read_file(log, &size);
    CHECK(recorded && !strcmp((const char*)recorded, events));
    free(recorded);
    free(output);
}
static void reset(const char* repo, const char* dump) {
    CHECK(!snx_remove_tree(repo));
    CHECK(!snx_mkdirs(repo));
    CHECK(!snx_write_file(dump, "synthetic", 9));
    set_env("SNX_WORKFLOW_FAIL", "");
}
int main(int argc, char** argv) {
    char *exe, *root, *repo, *dump, *log, *extracted, *game, *cwd, *expected_cwd;
    const char* args[3];
    char* output;
    int rc;
#ifndef _WIN32
    const char* modes[] = {"interrupt", "terminate", "stop", "parent"};
    int statuses[] = {128 + SIGINT, 128 + SIGTERM, 128 + SIGSTOP, 128 + SIGTERM};
    size_t i;
#endif
    if (argc == 3 && !strcmp(argv[1], "--probe")) {
        args[0] = argv[0];
        args[1] = NULL;
        if (!strcmp(argv[2], "exec")) args[0] = "/nonexistent/scarletnx-test";
        return snx_run_stage(args, !strcmp(argv[2], "cwd") ? "/nonexistent/scarletnx-test" : NULL, "Probe");
    }
    if (argc > 1) {
        rc = snx_driver(argc, argv);
        if (rc >= 0) return rc;
    }
    if (getenv("SNX_WORKFLOW_CHILD")) {
        const char* flag = getenv("SNX_WORKFLOW_HEADLESS");
        const char* fullscreen = getenv("SNX_WORKFLOW_FLAGS");
        if (fullscreen && (!strcmp(fullscreen, "--fullscreen") || !strcmp(fullscreen, "-f")))
            CHECK(argc == 2 && !strcmp(argv[1], "-f"));
        /* the game's own settings reach it as they were given */
        if (fullscreen && (!strncmp(fullscreen, "--touch=", 8) || !strncmp(fullscreen, "--controls=", 11)))
            CHECK(argc == 2 && !strcmp(argv[1], fullscreen));
        if (getenv("SNX_WORKFLOW_BACKEND") && *getenv("SNX_WORKFLOW_BACKEND")) CHECK(argc == 1);
        if (flag && *flag) CHECK(argc == 2 && !strcmp(argv[1], "-H"));
        if (getenv("SNX_WORKFLOW_ARGS") && *getenv("SNX_WORKFLOW_ARGS")) {
            CHECK(argc == 6 && !strcmp(argv[1], "--"));
            CHECK(!strcmp(argv[2], "some arg") && !strcmp(argv[3], ""));
            CHECK(!strcmp(argv[4], "-rx") && !strcmp(argv[5], "--headless=0"));
        }
        /* All game invocations must chdir, including repeat launches. */
        cwd = snx_absolute(".");
        expected_cwd = snx_absolute(getenv("SNX_WORKFLOW_GAMES"));
        CHECK(cwd && expected_cwd && !strcmp(cwd, expected_cwd));
        free(cwd);
        free(expected_cwd);
        return event('G');
    }
    exe = snx_executable(argv[0]);
    root = snx_temp_dir(NULL);
    CHECK(exe && root);
    repo = snx_path_join(root, "checkout with spaces");
    CHECK(!snx_mkdirs(repo));
    dump = snx_path_join(repo, "Test [game];.NSP");
    log = snx_path_join(root, "events");
    extracted = snx_path_join(repo, "work/Test game");
#ifdef _WIN32
    game = snx_path_join(repo, "games/Test game.exe");
#else
    game = snx_path_join(repo, "games/Test game");
#endif
    cwd = snx_path_join(repo, "games");
    set_env("SNX_WORKFLOW_GAMES", cwd);
    free(cwd);
    set_env("SNX_WORKFLOW_CHILD", "1");
    set_env("SNX_WORKFLOW_LOG", log);
    set_env("SNX_WORKFLOW_MODE", "exit");
    set_env("SNX_WORKFLOW_HEADLESS", "");
    set_env("SNX_WORKFLOW_PREFIX", "0");
    set_env("SNX_WORKFLOW_NRO", "");
    reset(repo, dump);
    run(exe, repo, dump, log, 0, "ERG", NULL);
    CHECK(snx_is_dir(extracted) && snx_is_file(game));
    run(exe, repo, dump, log, 0, "G", NULL);
    CHECK(!snx_remove_tree(game));
    run(exe, repo, dump, log, 0, "RG", NULL);
    set_env("SNX_WORKFLOW_FAIL", "G");
    run(exe, repo, dump, log, 7, "G", "Game failed (exit status 7)");
    set_env("SNX_WORKFLOW_FAIL", "");
    run(exe, repo, dump, log, 0, "G", NULL);

    /* Missing/mismatched/malformed ABI stamps are never trusted. */
    {
        char* stamp = snx_path_join(repo, "games/Test game.abi");
        CHECK(!snx_write_file(stamp, "3\n", 2));
        run(exe, repo, dump, log, 0, "RG", NULL);
        CHECK(!snx_write_file(stamp, "broken", 6));
        run(exe, repo, dump, log, 0, "RG", NULL);
        CHECK(!snx_write_file(stamp, "4", 1));
        run(exe, repo, dump, log, 0, "RG", NULL);
        CHECK(!snx_write_file(stamp, "4garbage\n", 9));
        run(exe, repo, dump, log, 0, "RG", NULL);
        CHECK(!snx_remove_tree(stamp));
        run(exe, repo, dump, log, 0, "RG", NULL);
        free(stamp);
    }
    {
        const char* flags[] = {"--fullscreen", "-f"};
        size_t j;
        for (j = 0; j < sizeof(flags) / sizeof(flags[0]); ++j) {
            set_env("SNX_WORKFLOW_FLAGS", flags[j]);
            run(exe, repo, dump, log, 0, "G", NULL);
        }
        set_env("SNX_WORKFLOW_FLAGS", "--touch=2");
        run(exe, repo, dump, log, 0, "G", NULL);
        set_env("SNX_WORKFLOW_FLAGS", "--controls=1");
        run(exe, repo, dump, log, 0, "G", NULL);
        set_env("SNX_WORKFLOW_FLAGS", "--fullscreen=invalid");
        run(exe, repo, dump, log, 1, "", "unknown option");
        set_env("SNX_WORKFLOW_FLAGS", "--fullscreen=0");
        run(exe, repo, dump, log, 1, "", "unknown option");
        set_env("SNX_WORKFLOW_FLAGS", "--fullscreen=1");
        run(exe, repo, dump, log, 1, "", "unknown option");
        set_env("SNX_WORKFLOW_FLAGS", "--backend=gcc");
        set_env("SNX_WORKFLOW_BACKEND", "gcc");
        run(exe, repo, dump, log, 0, "RG", NULL);
        set_env("SNX_WORKFLOW_FLAGS", "--backend=llvm");
        set_env("SNX_WORKFLOW_BACKEND", "llvm");
        run(exe, repo, dump, log, 0, "RG", NULL);
        set_env("SNX_WORKFLOW_BACKEND", "");
        set_env("SNX_WORKFLOW_FLAGS", "--backend=invalid");
        run(exe, repo, dump, log, 1, "", "unknown backend");
        set_env("SNX_WORKFLOW_FLAGS", "");
    }
    set_env("SNX_WORKFLOW_ARGS", "1");
    run(exe, repo, dump, log, 0, "G", NULL);
    set_env("SNX_WORKFLOW_ARGS", "");
    {
        const char* flags[] = {
            "-r",
            "--recomp",
#ifndef _WIN32
            "-rx",
            "-xr",
#endif
        };
        size_t j;
        for (j = 0; j < sizeof(flags) / sizeof(flags[0]); ++j) {
            set_env("SNX_WORKFLOW_FLAGS", flags[j]);
            reset(repo, dump);
            run(exe, repo, dump, log, 0, "ER", NULL);
            run(exe, repo, dump, log, 0, "R", NULL);
        }
        set_env("SNX_WORKFLOW_FLAGS", "");
    }
    reset(repo, dump);
    set_env("SNX_WORKFLOW_FAIL", "E");
    run(exe, repo, dump, log, 7, "E", "Extraction failed");
    CHECK(!snx_is_dir(extracted) && !snx_is_file(game));
    set_env("SNX_WORKFLOW_FAIL", "");
    run(exe, repo, dump, log, 0, "ERG", NULL);

    reset(repo, dump);
    set_env("SNX_WORKFLOW_FAIL", "R");
    run(exe, repo, dump, log, 7, "ER", "Recompilation failed");
    CHECK(snx_is_dir(extracted) && !snx_is_file(game));
    set_env("SNX_WORKFLOW_FAIL", "");
    run(exe, repo, dump, log, 0, "RG", NULL);
#ifndef _WIN32
    for (i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i) {
        reset(repo, dump);
        set_env("SNX_WORKFLOW_MODE", modes[i]);
        set_env("SNX_WORKFLOW_FAIL", "E");
        run(exe, repo, dump, log, statuses[i], "E", "Extraction interrupted by signal");
        CHECK(!snx_is_dir(extracted) && !snx_is_file(game));
        set_env("SNX_WORKFLOW_FAIL", "R");
        run(exe, repo, dump, log, statuses[i], "ER", "Recompilation interrupted by signal");
        CHECK(snx_is_dir(extracted) && !snx_is_file(game));
        set_env("SNX_WORKFLOW_FAIL", "");
        run(exe, repo, dump, log, 0, "RG", NULL);
        set_env("SNX_WORKFLOW_FAIL", "G");
        run(exe, repo, dump, log, statuses[i], "G", "Game interrupted by signal");
        set_env("SNX_WORKFLOW_FAIL", "");
        run(exe, repo, dump, log, 0, "G", NULL);
    }
#endif
    reset(repo, dump);
    set_env("SNX_WORKFLOW_MODE", "missing");
    set_env("SNX_WORKFLOW_FAIL", "E");
    run(exe, repo, dump, log, 1, "E", "Extraction finished without creating");
    set_env("SNX_WORKFLOW_FAIL", "R");
    run(exe, repo, dump, log, 1, "ER", "Recompilation finished without creating");
    set_env("SNX_WORKFLOW_FAIL", "");
    run(exe, repo, dump, log, 0, "RG", NULL);
    run(exe, repo, "/nonexistent/dump.xci", log, 1, "", "Input dump is missing");
    {
        const char* flags[] = {"--headless", "-H"};
        unsigned j;
        for (j = 0; j < 2; ++j) {
            set_env("SNX_WORKFLOW_HEADLESS", flags[j]);
            set_env("SNX_WORKFLOW_PREFIX", "0");
            run(exe, repo, dump, log, 0, "G", NULL);
            set_env("SNX_WORKFLOW_PREFIX", "1");
            reset(repo, dump);
            run(exe, repo, dump, log, 0, "ERG", NULL);
        }
        set_env("SNX_WORKFLOW_HEADLESS", "");
    }
    /* Decrypted containers share the extract/recompile checkpoints, including quoted dump paths. */
    {
        const char* names[] = {"My Switch DXCI's/Test [game];.dnsp", "My Switch DXCI's/Test [game];.dNsP",
                               "My Switch DXCI's/Test [game];.dxci", "My Switch DXCI's/Test [game];.dXcI"};
        unsigned j;
        for (j = 0; j < sizeof names / sizeof names[0]; ++j) {
            char *decrypted = snx_path_join(repo, names[j]), *directory = snx_path_join(repo, "My Switch DXCI's");
            CHECK(!snx_remove_tree(repo) && !snx_mkdirs(directory));
            CHECK(!snx_write_file(decrypted, "synthetic", 9));
            set_env("SNX_WORKFLOW_MODE", "exit");
            set_env("SNX_WORKFLOW_FAIL", "");
            run(exe, repo, decrypted, log, 0, "ERG", NULL);
            CHECK(snx_is_dir(extracted) && snx_is_file(game));
            run(exe, repo, decrypted, log, 0, "G", NULL);
            CHECK(!snx_remove_tree(game));
            run(exe, repo, decrypted, log, 0, "RG", NULL);
            /* -r prepares a decrypted dump too, by its extension */
            CHECK(!snx_remove_tree(extracted) && !snx_remove_tree(game));
            set_env("SNX_WORKFLOW_FLAGS", "-r");
            run(exe, repo, decrypted, log, 0, "ER", NULL);
            set_env("SNX_WORKFLOW_FLAGS", "");
            free(decrypted);
            free(directory);
        }
    }
    /* NROs bypass extraction even with a stale same-stem work directory or game executable. */
    {
        const char* names[] = {"Test [game];.nro", "Test [game];.NRO"};
        unsigned j;
        for (j = 0; j < 2; ++j) {
            char* nro = snx_path_join(repo, names[j]);
            reset(repo, nro);
            set_env("SNX_WORKFLOW_MODE", "exit");
            set_env("SNX_WORKFLOW_NRO", nro);
            set_env("SNX_WORKFLOW_HEADLESS", "--headless");
            run(exe, repo, nro, log, 0, "RG", NULL);
            CHECK(!snx_is_dir(extracted) && snx_is_file(game));
            CHECK(!snx_mkdirs(extracted));
            run(exe, repo, nro, log, 0, "RG", NULL);
            set_env("SNX_WORKFLOW_FAIL", "R");
            run(exe, repo, nro, log, 7, "R", "Recompilation failed");
            set_env("SNX_WORKFLOW_FAIL", "");
            run(exe, repo, nro, log, 0, "RG", NULL);
            CHECK(!snx_remove_tree(nro));
            run(exe, repo, nro, log, 1, "", "Input dump is missing");
            free(nro);
        }
        set_env("SNX_WORKFLOW_NRO", "");
        set_env("SNX_WORKFLOW_HEADLESS", "");
    }
    /* The probe takes a third argument, so use a separate, terminated vector. */
    {
        const char* probe[4];
        probe[0] = exe;
        probe[1] = "--probe";
        probe[2] = "cwd";
        probe[3] = NULL;
        output = NULL;
        rc = snx_run(probe, NULL, NULL, &output);
        CHECK(rc != 0 && strstr(output, "Probe"));
#ifndef _WIN32
        CHECK(strstr(output, "chdir to game directory"));
#endif
        free(output);
        probe[2] = "exec";
        output = NULL;
        rc = snx_run(probe, NULL, NULL, &output);
        CHECK(rc != 0 && strstr(output, "Probe"));
#ifndef _WIN32
        CHECK(strstr(output, "executing child process"));
#endif
        free(output);
    }
    CHECK(!snx_remove_tree(root));
    free(exe);
    free(root);
    free(repo);
    free(dump);
    free(log);
    free(extracted);
    free(game);
    printf("Driver workflow: %u checks passed\n", checks);
    return 0;
}
