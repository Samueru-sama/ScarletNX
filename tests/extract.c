/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Compile with src/common/utilities/common.c + extract.c. This executable also acts as a synthetic hactool. */
#define _POSIX_C_SOURCE 200809L
#include "nx/runtime/driver/extract.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <time.h>
#include <unistd.h>
#endif

static unsigned checks;
static const char prod_keys[] = "# synthetic, not console keys\n"
                                "good = 0123456789abcdef0123456789abcdef\n"
                                "long_value = aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n"
                                "bad_length = 123\n"
                                "not-a-name = 00000000000000000000000000000000\n"
                                "trailing = 00000000000000000000000000000000 garbage\n";
static const char filtered_keys[] = "good = 0123456789abcdef0123456789abcdef\n"
                                    "long_value = aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n";
static const char user_titles[] = "A0A0A0A0A0A0A0A0"
                                  "A0A0A0A0A0A0A0A0"
                                  " = 44444444444444444444444444444444\n"
                                  "a0a0a0a0a0a0a0a0"
                                  "a0a0a0a0a0a0a0a0"
                                  " = 55555555555555555555555555555555\n";

static void check(int good, const char* expression, int line) {
    checks++;
    if (!good) {
        fprintf(stderr, "extract.c:%d: check failed: %s\n", line, expression);
        exit(90);
    }
}
#define CHECK(value) check((value) != 0, #value, __LINE__)

static void set_env(const char* name, const char* value) {
#if defined(_WIN32)
    CHECK(!_putenv_s(name, value));
#else
    CHECK(!setenv(name, value, 1));
#endif
}

static char* read_text(const char* path) {
    unsigned char* bytes;
    size_t size;
    bytes = snx_read_file(path, &size);
    CHECK(bytes != NULL);
    return (char*)bytes;
}

/* Only the container signatures are real; the payload selects the synthetic hactool scenario. */
static void write_container(const char* path, const char* kind, const char* mode) {
    unsigned char bytes[0x1200];
    size_t size;
    memset(bytes, 0, sizeof bytes);
    if (!strcmp(kind, "pfs0"))
        memcpy(bytes, "PFS0", 4);
    else if (!strcmp(kind, "xci"))
        memcpy(bytes + 0x100, "HEAD", 4);
    else if (!strcmp(kind, "xci-keyarea")) /* the header after the card's key area */
        memcpy(bytes + 0x1100, "HEAD", 4);
    else if (!strcmp(kind, "dxci"))
        memcpy(bytes + 0x100, "DXCI", 4);
    size = strlen(mode);
    CHECK(size < 0x100);
    memcpy(bytes + 0x200, mode, size);
    CHECK(!snx_write_file(path, bytes, strcmp(kind, "xci-keyarea") ? 0x200 + size : sizeof bytes));
}

static char* container_mode(const char* path, int* xci) {
    unsigned char* bytes;
    char* mode;
    size_t size;
    bytes = snx_read_file(path, &size);
    CHECK(bytes != NULL && size >= 0x200);
    *xci = !memcmp(bytes + 0x100, "HEAD", 4) || !memcmp(bytes + 0x100, "DXCI", 4) ||
           (size >= 0x1104 && !memcmp(bytes + 0x1100, "HEAD", 4));
    CHECK(*xci || !memcmp(bytes, "PFS0", 4));
    mode = snx_strdup((const char*)bytes + 0x200);
    free(bytes);
    return mode;
}

static void pause_ms(long ms) {
#if defined(_WIN32)
    Sleep((DWORD)ms);
#else
    struct timespec t;
    t.tv_sec = ms / 1000;
    t.tv_nsec = ms % 1000 * 1000000L;
    while (nanosleep(&t, &t)) {}
#endif
}
/* size zero bytes added to path, in pieces a pause apart when slow: a tool writing for a while */
static void grow(const char* path, unsigned long size, int slow) {
    static const char zeros[65536];
    unsigned long left = size, piece;
    FILE* file;
    while (left) {
        piece = slow && left > size / 3 ? size / 3 : left;
        left -= piece;
        file = snx_fopen(path, "ab");
        CHECK(file != NULL);
        while (piece) {
            unsigned long n = piece < sizeof zeros ? piece : (unsigned long)sizeof zeros;
            CHECK(fwrite(zeros, 1, n, file) == n);
            piece -= n;
        }
        CHECK(!fclose(file));
        if (slow && left) pause_ms(600);
    }
}

static void write_child(const char* dir, const char* name, const void* content, size_t size) {
    char* path;
    path = snx_path_join(dir, name);
    CHECK(!snx_write_file(path, content, size));
    free(path);
}

static void write_nca(const char* dir, const char* name, const char* type, size_t size, int encrypted) {
    unsigned char bytes[2048];
    CHECK(size >= 0x204 && size <= sizeof bytes);
    memset(bytes, 0, sizeof bytes);
    memcpy(bytes, type, strlen(type));
    memcpy(bytes + 0x200, encrypted ? "\xfa\xfb\xfc\xfd" : "DNCA", 4);
    write_child(dir, name, bytes, size);
}

static void child_equals(const char* dir, const char* name, const char* expected) {
    char *path, *text;
    path = snx_path_join(dir, name);
    text = read_text(path);
    CHECK(!strcmp(text, expected));
    free(path);
    free(text);
}

static const char* find_arg(int argc, char** argv, const char* prefix) {
    int i;
    size_t size;
    size = strlen(prefix);
    for (i = 1; i < argc; i++)
        if (!strncmp(argv[i], prefix, size)) return argv[i] + size;
    return NULL;
}

static void ticket(const char* dir, unsigned index, unsigned right, unsigned value, int personalized) {
    static const unsigned signature_sizes[] = {0x23c, 0x13c, 0x7c, 0x23c, 0x13c, 0x7c, 0x3c};
    unsigned char bytes[1024];
    char name[40];
    unsigned signature;
    size_t offset, size;
    memset(bytes, 0, sizeof bytes);
    signature = 0x10000U + index;
    bytes[0] = (unsigned char)signature;
    bytes[1] = (unsigned char)(signature >> 8);
    bytes[2] = (unsigned char)(signature >> 16);
    bytes[3] = (unsigned char)(signature >> 24);
    offset = 4 + signature_sizes[index];
    size = offset + 0x170;
    memset(bytes + offset + 0x160, (int)right, 16);
    memset(bytes + offset + 0x40, (int)value, 16);
    bytes[offset + 0x141] = (unsigned char)personalized;
    sprintf(name, "%u-%u.tik", index, right);
    write_child(dir, name, bytes, size);
}

static void expected_titles(const char* home, int update) {
    char *dir, *path, *text, name[33], value[33], line[72];
    unsigned i, j;
    size_t count;
    dir = snx_path_join(home, ".switch");
    path = snx_path_join(dir, "title.keys");
    text = read_text(path);
    count = 0;
    for (i = 0; text[i]; i++)
        if (text[i] == '\n') count++;
    CHECK(count == (size_t)(update ? 8 : 7));
    for (i = 0; i < (unsigned)(update ? 8 : 7); i++) {
        for (j = 0; j < 16; j++) {
            sprintf(name + j * 2, "%02x", i == 7 ? 0xb0U : 0xa0U + i);
            sprintf(value + j * 2, "%02x", i == 7 ? 0xb1U : i ? 0x90U + i : 0x44U);
        }
        sprintf(line, "%s = %s\n", name, value);
        CHECK(strstr(text, line) != NULL);
    }
    CHECK(strstr(text, "55555555555555555555555555555555") == NULL);
    free(text);
    free(path);
    free(dir);
}

static int fake_hactool(int argc, char** argv) {
    const char *home, *profile, *dir, *exefs, *romfs, *base, *report, *no_keys;
    char *keys, *mode_path, *mode, *input;
    FILE* file;
    unsigned i;
    int update, xci, encrypted, embedded, update_path;
    CHECK(argc >= 3 && !strcmp(argv[1], "--disablekeywarns"));
    home = getenv("HOME");
    profile = getenv("USERPROFILE");
    CHECK(home && profile && !strcmp(home, profile));
    CHECK(getenv("XDG_CONFIG_HOME") == NULL);
    keys = snx_path_join(home, ".switch");
    no_keys = getenv("SNX_EXTRACT_TEST_NO_KEYS");
    child_equals(keys, "prod.keys", no_keys && !strcmp(no_keys, "1") ? "" : filtered_keys);
    free(keys);
    report = getenv("SNX_EXTRACT_TEST_REPORT");
    CHECK(report != NULL);
    file = snx_fopen(report, "ab");
    CHECK(file != NULL && fprintf(file, "%s\n", home) > 0 && !fclose(file));
    mode_path = snx_path_join(home, "mode");
    dir = find_arg(argc, argv, "--outdir=");
    if (!dir) dir = find_arg(argc, argv, "--securedir=");
    if (dir) {
        input = container_mode(argv[argc - 1], &xci);
        update = !strcmp(input, "update");
        if (!update) CHECK(!snx_write_file(mode_path, input, strlen(input)));
        CHECK(!strcmp(argv[2], xci ? "-txci" : "-tpfs0"));
        CHECK(find_arg(argc, argv, xci ? "--securedir=" : "--outdir=") != NULL);
        if (!strcmp(input, "failure")) {
            puts("key SECRET-EXTRACT-OUTPUT-MUST-NOT-ESCAPE");
            fputs("0123456789abcdef0123456789abcdef secret without a keyword\n", stderr);
            free(input);
            free(mode_path);
            return 23;
        }
        CHECK(!snx_mkdirs(dir));
        encrypted = !strcmp(input, "encrypted-no-keys");
        write_nca(dir, "small.nca", !strcmp(input, "dlc") ? "Data" : "Program", 0x240, encrypted);
        write_nca(dir, "large.nca", !strcmp(input, "dlc") ? "Control" : "Program", 0x300, encrypted);
        if (strcmp(input, "no-control")) write_nca(dir, "control.nca", "Control", 0x400, encrypted);
        write_nca(dir, "ignored.cnmt.nca", "Program", 0x600, encrypted);
        if (!strncmp(input, "slow", 4)) { /* the program NCA 3 MiB, and a while writing another 3 */
            char* path = snx_path_join(dir, "large.nca");
            grow(path, 3UL << 20, 0);
            free(path);
            path = snx_path_join(dir, "big.bin");
            grow(path, 3UL << 20, 1);
            free(path);
        }
        if (update) {
            ticket(dir, 4, 0xa1, 0xff, 0);
            ticket(dir, 4, 0xb0, 0xb1, 0);
        } else {
            for (i = 0; i < 7; i++) ticket(dir, i, 0xa0U + i, 0x90U + i, 0);
            ticket(dir, 4, 0xee, 0xee, 1);
            write_child(dir, "broken.tik", "bad", 3);
        }
        free(input);
    } else if (!strcmp(argv[2], "-i")) {
        CHECK(strstr(argv[3], ".cnmt.nca") == NULL);
        mode = read_text(mode_path);
        expected_titles(home, !strcmp(mode, "base-update") || !strcmp(mode, "dnsp-update") ||
                                  !strcmp(mode, "dxci-update") || !strcmp(mode, "embedded-update-explicit") ||
                                  !strcmp(mode, "unmatched-update"));
        input = read_text(argv[3]);
        printf("Content Type: %s\n", !strncmp(input, "Program", 7) ? "Program" : "Control");
        embedded = !strcmp(mode, "embedded-update") || !strcmp(mode, "embedded-update-explicit") ||
                   !strcmp(mode, "unrelated-embedded-update");
        update_path = strstr(argv[3], "/update/") != NULL || strstr(argv[3], "\\update\\") != NULL;
        if (update_path || (embedded && strstr(argv[3], "large.nca")))
            printf("Title ID: %s\n", !strcmp(mode, "unmatched-update") || !strcmp(mode, "unrelated-embedded-update")
                                         ? "0100000000001800"
                                         : "0100000000000800");
        else
            puts("Title ID: 0100000000000000");
        free(input);
        free(mode);
    } else if (!find_arg(argc, argv, "--exefsdir=") && find_arg(argc, argv, "--romfsdir=")) {
        /* the Control NCA's RomFS: its control.nacp, the game's name first of 16 languages; the update's its own */
        unsigned char nacp[0x4000];
        const char* name = strstr(argv[argc - 1], "/update/") || strstr(argv[argc - 1], "\\update\\")
                               ? "Synthetic Game Updated"
                               : "Synthetic Game"; /* the scenario's own folder may be named for an update */
        romfs = find_arg(argc, argv, "--romfsdir=");
        CHECK(strstr(argv[argc - 1], "control.nca") != NULL && !snx_mkdirs(romfs));
        memset(nacp, 0, sizeof nacp);
        memcpy(nacp, name, strlen(name));
        write_child(romfs, "control.nacp", (const char*)nacp, sizeof nacp);
    } else {
        exefs = find_arg(argc, argv, "--exefsdir=");
        romfs = find_arg(argc, argv, "--romfsdir=");
        base = find_arg(argc, argv, "--basenca=");
        mode = read_text(mode_path);
        CHECK(exefs && romfs &&
              strstr(argv[argc - 1], !strcmp(mode, "unrelated-embedded-update") ? "small.nca" : "large.nca"));
        embedded = !strcmp(mode, "embedded-update") || !strcmp(mode, "embedded-update-explicit");
        update =
            embedded || !strcmp(mode, "base-update") || !strcmp(mode, "dnsp-update") || !strcmp(mode, "dxci-update");
        CHECK((update && base && strstr(base, embedded ? "small.nca" : "large.nca")) || (!update && !base));
        if (!strcmp(mode, "embedded-update-explicit")) {
            CHECK(strstr(base, "base") != NULL);
            CHECK(strstr(argv[argc - 1], "update") != NULL);
        }
        CHECK(!snx_mkdirs(exefs) && !snx_mkdirs(romfs));
        if (strcmp(mode, "no-main")) write_child(exefs, "main", "NSO0 synthetic", 14);
        write_child(romfs, "asset", update ? "updated" : "base", update ? 7 : 4);
        if (!strncmp(mode, "slow", 4)) {
            char* path = snx_path_join(romfs, "big");
            grow(path, 3UL << 20, 1);
            free(path);
        }
        free(mode);
    }
    free(mode_path);
    return 0;
}

static void assert_temp_removed(const char* report) {
    char *text, *line, *end;
    text = read_text(report);
    line = text;
    while (*line) {
        end = strchr(line, '\n');
        CHECK(end != NULL);
        *end = 0;
        CHECK(!snx_is_dir(line));
        line = end + 1;
    }
    free(text);
}

static int fake_cmake(int argc, char** argv) {
    char *self, *tool;
    if (argc == 6 && !strcmp(argv[1], "--preset")) {
        CHECK(!strcmp(argv[2], "default") && !strcmp(argv[3], "-B"));
        CHECK(!strcmp(argv[5], "-DSNX_HACTOOL_ONLY=ON"));
        CHECK(!snx_mkdirs(argv[4]));
        write_child(argv[4], "configured", "yes", 3);
        return 0;
    }
    CHECK(argc == 5 && !strcmp(argv[1], "--build") && !strcmp(argv[3], "--target"));
    CHECK(!strcmp(argv[4], "hactool"));
    child_equals(argv[2], "configured", "yes");
    self = snx_executable(argv[0]);
#if defined(_WIN32)
    tool = snx_path_join(argv[2], "hactool.exe");
#else
    tool = snx_path_join(argv[2], "hactool");
#endif
    CHECK(!snx_copy_file(self, tool) && !snx_make_executable(tool));
    free(tool);
    free(self);
    return 0;
}

static int scenario(const char* self, const char* scratch, const char* mode) {
    SnxPaths paths;
    char *repo, *build, *home, *switch_dir, *prod, *titles, *base, *patch, *out, *tool, *report, *exefs, *text;
    char *arguments[16], *out_argument;
    unsigned char *original, *after;
    size_t original_size, after_size;
    const char *base_name, *kind, *payload;
    int count, result, expected_failure, existing, explicit_keys, no_keys, invalid, apply_update, merged;
    repo = snx_path_join(scratch, "repo space & ()");
    build = snx_path_join(repo, "build space");
    home = snx_path_join(scratch, "original home");
    switch_dir = snx_path_join(home, ".switch");
    CHECK(!snx_mkdirs(build) && !snx_mkdirs(switch_dir));
    paths.repo = repo;
    paths.build = build;
    paths.executable = self;
    /* hactool is built in the checkout's build/extract when it is missing, never without a checkout */
    paths.source = strcmp(mode, "autobuild-no-checkout") ? repo : NULL;
#if defined(_WIN32)
    tool = snx_path_join(build, "hactool.exe");
#else
    tool = snx_path_join(build, "hactool");
#endif
    if (!strncmp(mode, "autobuild", 9)) {
        free(tool);
#if defined(_WIN32)
        tool = snx_path_join(build, "cmake.exe");
        text = snx_format("%s;%s", build, getenv("PATH") ? getenv("PATH") : "");
#else
        tool = snx_path_join(build, "cmake");
        text = snx_format("%s:%s", build, getenv("PATH") ? getenv("PATH") : "");
#endif
        set_env("PATH", text);
        free(text);
    }
    CHECK(!snx_copy_file(self, tool) && !snx_make_executable(tool));
    explicit_keys = !strcmp(mode, "explicit") || !strcmp(mode, "explicit-missing-keys");
    merged = !strcmp(mode, "embedded-update") || !strcmp(mode, "embedded-update-explicit") ||
             !strcmp(mode, "unrelated-embedded-update");
    no_keys = strstr(mode, "keyless") != NULL || !strcmp(mode, "encrypted-no-keys") ||
              !strcmp(mode, "explicit-missing-keys") || merged;
    invalid = !strcmp(mode, "malformed-dxci") || !strcmp(mode, "truncated-dxci") || !strcmp(mode, "invalid-update");
    apply_update = !strcmp(mode, "base-update") || !strcmp(mode, "dnsp-update") || !strcmp(mode, "dxci-update") ||
                   !strcmp(mode, "embedded-update-explicit") || !strcmp(mode, "invalid-update") ||
                   !strcmp(mode, "unmatched-update");
    prod = snx_path_join(explicit_keys || !strcmp(mode, "homekeys") ? switch_dir : repo, "prod.keys");
    titles = snx_path_join(explicit_keys || !strcmp(mode, "homekeys") ? switch_dir : repo, "title.keys");
    if (!no_keys) CHECK(!snx_write_file(prod, prod_keys, strlen(prod_keys)));
    CHECK(!snx_write_file(titles, user_titles, strlen(user_titles)));
    set_env("SNX_EXTRACT_TEST_NO_KEYS", no_keys ? "1" : "0");
    set_env("HOME", home);
    set_env("USERPROFILE", home);
    set_env("XDG_CONFIG_HOME", "must not reach hactool");
    kind = "pfs0";
    base_name = "Base Game (&).NSP";
    if (!strcmp(mode, "xci") || !strcmp(mode, "xci-keyless") || !strcmp(mode, "xci-renamed-dnsp") ||
        !strcmp(mode, "slow-xci") || !strcmp(mode, "xci-keyarea")) {
        kind = !strcmp(mode, "xci-keyarea") ? "xci-keyarea" : "xci";
        base_name = !strcmp(mode, "xci-renamed-dnsp") ? "Base Game (&).DNsP" : "Base Game (&).XCI";
    } else if (strstr(mode, "dxci") || merged) {
        kind = "dxci";
        base_name = !strcmp(mode, "dxci") ? "Base Game (&).dxci" : "Base Game (&).dXcI";
        if (!strcmp(mode, "pfs0-renamed-dxci")) kind = "pfs0";
        if (!strcmp(mode, "dxci-renamed-nsp")) base_name = "Base Game (&).NSP";
    } else if (strstr(mode, "dnsp") || !strcmp(mode, "encrypted-no-keys"))
        base_name = !strcmp(mode, "dnsp") ? "Base Game (&).dnsp" : "Base Game (&).dNsP";
    payload = "base";
    if (!strncmp(mode, "slow", 4) || !strcmp(mode, "failure") || !strcmp(mode, "no-main") || !strcmp(mode, "dlc") ||
        !strcmp(mode, "base-update") || !strcmp(mode, "dnsp-update") || !strcmp(mode, "dxci-update") ||
        !strcmp(mode, "encrypted-no-keys") || !strcmp(mode, "unmatched-update") || !strcmp(mode, "no-control") ||
        merged)
        payload = mode;
    if (!strcmp(mode, "preserve") || !strcmp(mode, "dot-parent")) payload = "no-main";
    if (!strcmp(mode, "malformed-dxci")) kind = "invalid";
    base = snx_path_join(scratch, "My Switch DXCI's");
    CHECK(!snx_mkdirs(base));
    text = snx_path_join(base, base_name);
    free(base);
    base = text;
    if (!strcmp(mode, "truncated-dxci"))
        CHECK(!snx_write_file(base, "DXCI", 4));
    else
        write_container(base, kind, payload);
    if (!strcmp(mode, "slow")) grow(base, 3UL << 20, 0);
    if (!strcmp(mode, "slow-xci")) { /* 8 MiB, of which the root HFS0 at 0x400 says the secure partition is 3 */
        unsigned char root[0x10 + 2 * 0x40 + 14], at[8];
        FILE* file;
        memset(root, 0, sizeof root);
        memcpy(root, "HFS0", 4);
        root[4] = 2;                               /* files */
        root[8] = 14;                              /* the names' bytes */
        root[0x10 + 8 + 2] = 0x10;                 /* normal: 1 MiB, its name at 0 */
        root[0x50 + 2] = 0x10;                     /* secure: at 1 MiB, */
        root[0x50 + 8 + 2] = 0x30;                 /* 3 MiB, */
        root[0x50 + 0x10] = 7;                     /* its name at 7 */
        memcpy(root + 0x90, "normal\0secure", 14); /* both NUL-terminated */
        memset(at, 0, sizeof at);
        at[1] = 4; /* 0x400 */
        file = snx_fopen(base, "r+b");
        CHECK(file != NULL && !fseek(file, 0x130, SEEK_SET) && fwrite(at, 1, 8, file) == 8);
        CHECK(!fseek(file, 0x400, SEEK_SET) && fwrite(root, 1, sizeof root, file) == sizeof root && !fclose(file));
        grow(base, 8UL << 20, 0);
    }
    original = snx_read_file(base, &original_size);
    CHECK(original != NULL);
    patch = snx_path_join(scratch, !strcmp(mode, "dxci-update") ? "Update file.DXCI" : "Update file.dnsp");
    write_container(patch,
                    !strcmp(mode, "invalid-update") ? "invalid"
                    : !strcmp(mode, "dxci-update")  ? "dxci"
                                                    : "pfs0",
                    "update");
    out =
        !strcmp(mode, "named-output") ? snx_path_join(repo, "work/Named Game") : snx_path_join(scratch, "output space");
    out_argument = !strcmp(mode, "dot-parent") ? snx_path_join(out, "missing/..") : snx_strdup(out);
    report = snx_path_join(scratch, "report.txt");
    set_env("SNX_EXTRACT_TEST_REPORT", report);
    existing = !strcmp(mode, "existing") || !strcmp(mode, "preserve") || !strcmp(mode, "dot-parent");
    exefs = snx_path_join(out, "exefs");
    if (existing) {
        CHECK(!snx_mkdirs(out));
        write_child(out, "keep.txt", "owner data", 10);
    }
    if (!strcmp(mode, "existing")) CHECK(!snx_mkdirs(exefs));
    count = 0;
    arguments[count++] = base;
    if (!strcmp(mode, "named-output"))
        arguments[count++] = "--name=Named Game";
    else {
        arguments[count++] = "--out";
        arguments[count++] = out_argument;
    }
    if (apply_update) {
        arguments[count++] = "--update";
        arguments[count++] = patch;
    }
    if (explicit_keys) {
        arguments[count++] = "--keys";
        arguments[count++] = prod;
        arguments[count++] = "--title-keys";
        arguments[count++] = titles;
    }
    expected_failure = !strcmp(mode, "failure") || !strcmp(mode, "no-main") || !strcmp(mode, "dlc") ||
                       !strcmp(mode, "encrypted-no-keys") || !strcmp(mode, "explicit-missing-keys") ||
                       !strcmp(mode, "unmatched-update") || !strcmp(mode, "autobuild-no-checkout") || existing ||
                       invalid;
    result = snx_extract(count, arguments, &paths);
    CHECK((result != 0) == expected_failure);
    if (!strcmp(mode, "autobuild-no-checkout")) {
        text = snx_path_join(repo, "build/extract");
        CHECK(!snx_is_dir(text)); /* CMake did not run */
        free(text);
    }
    after = snx_read_file(base, &after_size);
    CHECK(after != NULL && after_size == original_size && !memcmp(original, after, original_size));
    free(original);
    free(after);
    if (no_keys)
        CHECK(!snx_is_file(prod));
    else
        child_equals(explicit_keys || !strcmp(mode, "homekeys") ? switch_dir : repo, "prod.keys", prod_keys);
    child_equals(explicit_keys || !strcmp(mode, "homekeys") ? switch_dir : repo, "title.keys", user_titles);
    if (existing)
        child_equals(out, "keep.txt", "owner data");
    else if (expected_failure)
        CHECK(!snx_is_dir(out) && !snx_is_file(out));
    else {
        child_equals(exefs, "main", "NSO0 synthetic");
        text = snx_path_join(out, "romfs");
        child_equals(text, "asset",
                     apply_update || (merged && strcmp(mode, "unrelated-embedded-update")) ? "updated" : "base");
        free(text);
        /* the game's name beside exefs/, from the update's Control NCA when there is one */
        text = snx_path_join(out, "control.nacp");
        if (!strcmp(mode, "no-control"))
            CHECK(!snx_path_exists(text));
        else {
            size_t size;
            unsigned char* nacp = (unsigned char*)snx_read_file(text, &size);
            const char* name = apply_update ? "Synthetic Game Updated" : "Synthetic Game";
            CHECK(nacp && size == 0x4000 && !memcmp(nacp, name, strlen(name)) && !nacp[strlen(name)]);
            free(nacp);
        }
        free(text);
    }
    if (!strcmp(mode, "existing") || !strcmp(mode, "explicit-missing-keys") || !strcmp(mode, "autobuild-no-checkout") ||
        invalid)
        CHECK(!snx_is_file(report)); /* hactool never ran */
    else
        assert_temp_removed(report);
    CHECK(getenv("XDG_CONFIG_HOME") && !strcmp(getenv("XDG_CONFIG_HOME"), "must not reach hactool"));
    free(repo);
    free(build);
    free(home);
    free(switch_dir);
    free(prod);
    free(titles);
    free(base);
    free(patch);
    free(out);
    free(out_argument);
    free(tool);
    free(report);
    free(exefs);
    printf("PASS extraction %s (%u checks)\n", mode, checks);
    return 0;
}

int main(int argc, char** argv) {
    static const char* modes[] = {"base",
                                  "xci",
                                  "xci-keyarea",
                                  "dnsp",
                                  "dnsp-mixed",
                                  "dxci",
                                  "dxci-mixed",
                                  "dnsp-keyless",
                                  "dxci-keyless",
                                  "xci-keyless",
                                  "xci-renamed-dnsp",
                                  "pfs0-renamed-dxci",
                                  "dxci-renamed-nsp",
                                  "base-update",
                                  "dnsp-update",
                                  "dxci-update",
                                  "embedded-update",
                                  "embedded-update-explicit",
                                  "unrelated-embedded-update",
                                  "unmatched-update",
                                  "explicit",
                                  "homekeys",
                                  "failure",
                                  "no-main",
                                  "dlc",
                                  "existing",
                                  "preserve",
                                  "dot-parent",
                                  "autobuild",
                                  "autobuild-no-checkout",
                                  "named-output",
                                  "malformed-dxci",
                                  "truncated-dxci",
                                  "invalid-update",
                                  "encrypted-no-keys",
                                  "no-control",
                                  "explicit-missing-keys",
                                  "slow",
                                  "slow-xci"};
    const char* command[6];
    char *self, *scratch, *directory, *output, *marker, *text;
    size_t i;
    int result;
    if (argc > 1 && !strcmp(argv[1], "--disablekeywarns")) return fake_hactool(argc, argv);
    if (argc > 1 && (!strcmp(argv[1], "--preset") || !strcmp(argv[1], "--build"))) return fake_cmake(argc, argv);
    self = snx_executable(argv[0]);
    CHECK(self != NULL);
    if (argc == 4 && !strcmp(argv[1], "--scenario")) {
        result = scenario(self, argv[2], argv[3]);
        free(self);
        return result;
    }
    CHECK(argc == 1);
    scratch = snx_temp_dir(NULL);
    CHECK(scratch != NULL);
    marker = snx_path_join(scratch, "extract-test.marker");
    CHECK(!snx_write_file(marker, "synthetic extraction test", 25));
    for (i = 0; i < sizeof modes / sizeof modes[0]; i++) {
        directory = snx_path_join(scratch, modes[i]);
        CHECK(!snx_mkdirs(directory));
        command[0] = self;
        command[1] = "--scenario";
        command[2] = directory;
        command[3] = modes[i];
        command[4] = NULL;
        output = NULL;
        result = snx_run(command, NULL, NULL, &output);
        CHECK(output != NULL);
        CHECK(strstr(output, "SECRET-EXTRACT-OUTPUT-MUST-NOT-ESCAPE") == NULL);
        CHECK(strstr(output, "0123456789abcdef0123456789abcdef") == NULL);
        if (result) fprintf(stderr, "%s", output);
        CHECK(result == 0);
        if (!strcmp(modes[i], "malformed-dxci") || !strcmp(modes[i], "truncated-dxci") ||
            !strcmp(modes[i], "invalid-update"))
            CHECK(strstr(output, "Unsupported container header") != NULL);
        if (!strcmp(modes[i], "encrypted-no-keys")) CHECK(strstr(output, "Encrypted content needs prod.keys") != NULL);
        if (!strcmp(modes[i], "unmatched-update"))
            CHECK(strstr(output, "Update has no Program NCA matching the selected base game") != NULL);
        if (!strcmp(modes[i], "explicit-missing-keys"))
            CHECK(strstr(output, "Specified prod.keys file is missing") != NULL);
        /* a game without a Control NCA keeps the name of its file, and says why */
        CHECK(!strstr(output, "The game keeps the name of its file") == !!strcmp(modes[i], "no-control"));
        if (!strcmp(modes[i], "no-control"))
            CHECK(strstr(output, "The game keeps the name of its file: it has no Control NCA") != NULL);
        /* a hactool of seconds: a row a second, each over the last (a carriage return), the last one ended; an XCI's
           total its secure partition, not the file's padding */
        if (!strncmp(modes[i], "slow", 4)) {
            CHECK(strstr(output, "%)\r  unpacked ") && (strstr(output, "  unpacked 3 of 3 MB (100%)\n") ||
                                                        strstr(output, "  unpacked 3 of 3 MB (100%)\r\n")));
            /* the program NCA is its content and its headers: 99% of it */
            CHECK(strstr(output, "%)\r  extracted ") && strstr(output, "  extracted 3 of 3 MB (99%)"));
        } else
            CHECK(!strstr(output, "  unpacked ") && !strstr(output, "  extracted ")); /* a quick one says nothing */
        printf("PASS extraction scenario %s\n", modes[i]);
        free(output);
        free(directory);
    }
    text = read_text(marker);
    CHECK(!strcmp(text, "synthetic extraction test"));
    free(text);
    CHECK(strstr(scratch, ".snx-") != NULL);
    CHECK(!snx_remove_tree(scratch));
    free(marker);
    free(scratch);
    free(self);
    printf("PASS %u extraction driver checks; synthetic hactool, no dumps or real keys\n", checks);
    return 0;
}
