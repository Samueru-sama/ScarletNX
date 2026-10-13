/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Compile with src/common/utilities/common.c + extract.c. This executable also acts as a synthetic hactool. */
#define _POSIX_C_SOURCE 200809L
#include "extract.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
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

static void write_child(const char* dir, const char* name, const void* content, size_t size) {
    char* path;
    path = snx_path_join(dir, name);
    CHECK(!snx_write_file(path, content, size));
    free(path);
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
    const char *home, *profile, *dir, *exefs, *romfs, *base, *report;
    char *keys, *mode_path, *mode, *input;
    unsigned char large[128], control[256], meta[512];
    FILE* file;
    unsigned i;
    int update;
    CHECK(argc >= 3 && !strcmp(argv[1], "--disablekeywarns"));
    home = getenv("HOME");
    profile = getenv("USERPROFILE");
    CHECK(home && profile && !strcmp(home, profile));
    CHECK(getenv("XDG_CONFIG_HOME") == NULL);
    keys = snx_path_join(home, ".switch");
    child_equals(keys, "prod.keys", filtered_keys);
    free(keys);
    report = getenv("SNX_EXTRACT_TEST_REPORT");
    CHECK(report != NULL);
    file = snx_fopen(report, "ab");
    CHECK(file != NULL && fprintf(file, "%s\n", home) > 0 && !fclose(file));
    mode_path = snx_path_join(home, "mode");
    dir = find_arg(argc, argv, "--outdir=");
    if (!dir) dir = find_arg(argc, argv, "--securedir=");
    if (dir) {
        input = read_text(argv[argc - 1]);
        update = !strcmp(input, "update");
        if (!update) CHECK(!snx_write_file(mode_path, input, strlen(input)));
        CHECK((!strcmp(input, "xci") && !strcmp(argv[2], "-txci")) ||
              (strcmp(input, "xci") && !strcmp(argv[2], "-tpfs0")));
        if (!strcmp(input, "failure")) {
            puts("key SECRET-EXTRACT-OUTPUT-MUST-NOT-ESCAPE");
            fputs("0123456789abcdef0123456789abcdef secret without a keyword\n", stderr);
            free(input);
            free(mode_path);
            return 23;
        }
        CHECK(!snx_mkdirs(dir));
        memset(large, ' ', sizeof large);
        memset(control, ' ', sizeof control);
        memset(meta, ' ', sizeof meta);
        memcpy(large, !strcmp(input, "dlc") ? "Control" : "Program", 7);
        memcpy(control, "Control", 7);
        memcpy(meta, "Program", 7);
        write_child(dir, "small.nca", !strcmp(input, "dlc") ? "Data" : "Program", !strcmp(input, "dlc") ? 4 : 7);
        write_child(dir, "large.nca", large, sizeof large);
        write_child(dir, "control.nca", control, sizeof control);
        write_child(dir, "ignored.cnmt.nca", meta, sizeof meta);
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
        expected_titles(home, !strcmp(mode, "base-update"));
        input = read_text(argv[3]);
        printf("Content Type: %s\n", !strncmp(input, "Program", 7) ? "Program" : "Control");
        free(input);
        free(mode);
    } else {
        exefs = find_arg(argc, argv, "--exefsdir=");
        romfs = find_arg(argc, argv, "--romfsdir=");
        base = find_arg(argc, argv, "--basenca=");
        mode = read_text(mode_path);
        CHECK(exefs && romfs && strstr(argv[argc - 1], "large.nca"));
        update = !strcmp(mode, "base-update");
        CHECK((update && base && strstr(base, "large.nca")) || (!update && !base));
        CHECK(!snx_mkdirs(exefs) && !snx_mkdirs(romfs));
        if (strcmp(mode, "no-main")) write_child(exefs, "main", "NSO0 synthetic", 14);
        write_child(romfs, "asset", update ? "updated" : "base", update ? 7 : 4);
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
    int count, result, expected_failure, existing, explicit_keys;
    repo = snx_path_join(scratch, "repo space & ()");
    build = snx_path_join(repo, "build space");
    home = snx_path_join(scratch, "original home");
    switch_dir = snx_path_join(home, ".switch");
    CHECK(!snx_mkdirs(build) && !snx_mkdirs(switch_dir));
    paths.repo = repo;
    paths.build = build;
    paths.executable = self;
#if defined(_WIN32)
    tool = snx_path_join(build, "hactool.exe");
#else
    tool = snx_path_join(build, "hactool");
#endif
    if (!strcmp(mode, "autobuild")) {
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
    explicit_keys = !strcmp(mode, "explicit");
    prod = snx_path_join(explicit_keys || !strcmp(mode, "homekeys") ? switch_dir : repo, "prod.keys");
    titles = snx_path_join(explicit_keys || !strcmp(mode, "homekeys") ? switch_dir : repo, "title.keys");
    CHECK(!snx_write_file(prod, prod_keys, strlen(prod_keys)));
    CHECK(!snx_write_file(titles, user_titles, strlen(user_titles)));
    set_env("HOME", home);
    set_env("USERPROFILE", home);
    set_env("XDG_CONFIG_HOME", "must not reach hactool");
    base = snx_path_join(scratch, !strcmp(mode, "xci") ? "Base Game (&).XCI" : "Base Game (&).NSP");
    text =
        !strcmp(mode, "explicit") || !strcmp(mode, "homekeys") || !strcmp(mode, "existing") || !strcmp(mode, "preserve")
            ? "base"
            : (char*)mode;
    if (!strcmp(mode, "autobuild") || !strcmp(mode, "named-output")) text = "base";
    if (!strcmp(mode, "preserve") || !strcmp(mode, "dot-parent")) text = "no-main";
    CHECK(!snx_write_file(base, text, strlen(text)));
    patch = snx_path_join(scratch, "Update file.nsp");
    CHECK(!snx_write_file(patch, "update", 6));
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
    if (!strcmp(mode, "base-update")) {
        arguments[count++] = "--update";
        arguments[count++] = patch;
    }
    if (explicit_keys) {
        arguments[count++] = "--keys";
        arguments[count++] = prod;
        arguments[count++] = "--title-keys";
        arguments[count++] = titles;
    }
    expected_failure = !strcmp(mode, "failure") || !strcmp(mode, "no-main") || !strcmp(mode, "dlc") || existing;
    result = snx_extract(count, arguments, &paths);
    CHECK((result != 0) == expected_failure);
    child_equals(explicit_keys || !strcmp(mode, "homekeys") ? switch_dir : repo, "prod.keys", prod_keys);
    child_equals(explicit_keys || !strcmp(mode, "homekeys") ? switch_dir : repo, "title.keys", user_titles);
    if (existing)
        child_equals(out, "keep.txt", "owner data");
    else if (expected_failure)
        CHECK(!snx_is_dir(out) && !snx_is_file(out));
    else {
        child_equals(exefs, "main", "NSO0 synthetic");
        text = snx_path_join(out, "romfs");
        child_equals(text, "asset", !strcmp(mode, "base-update") ? "updated" : "base");
        free(text);
    }
    if (!strcmp(mode, "existing"))
        CHECK(!snx_is_file(report));
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
    static const char* modes[] = {"base",       "xci",       "base-update", "explicit", "homekeys",
                                  "failure",    "no-main",   "dlc",         "existing", "preserve",
                                  "dot-parent", "autobuild", "named-output"};
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
