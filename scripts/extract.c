/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright 2026 the ScarletNX contributors
 * cc -std=c90 -pedantic scripts/extract.c scripts/portable.c -o snx-extract
 * XCI/NSP + optional update -> exefs/ + romfs/ via hactool.
 * MinGW/llvm-mingw: add -lshell32. CMake: scripts/CMakeLists.txt.
 * Usage: snx-extract --help
 */
#include "portable.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static char* temporary;
static char* output;
static int made_output;
static int succeeded;
static char* hactool;
static char* isolated_home;

static void cleanup(void) {
    /* Clear ownership before cleanup: allocation failure must not retry deletion. */
    char* tmp;
    char* out;
    tmp = temporary;
    out = (!succeeded && made_output) ? output : NULL;
    temporary = NULL;
    made_output = 0;
    if (tmp) remove_tree(tmp);
    if (out) remove_tree(out);
}

static void usage(void) {
    puts("Usage: snx-extract BASE [--update NSP] [--name NAME] [--out DIR]\n"
         "                   [--keys FILE] [--title-keys FILE]\n\n"
         "XCI/NSP -> work/NAME/{exefs,romfs}; NAME defaults to BASE stem.\n"
         "--out overrides output; --update must match the base title.\n"
         "Keys: prod.keys + title.keys; checkout root, then home/.switch/.\n");
    puts("hactool: build/default/hactool (auto-built if missing).\n"
         "Failure removes temp files + newly created output.\n"
         "SNX_REPO: checkout override.");
}

static char* find_keys(const char* given, const char* name, const char* repo) {
    char* root;
    char* home;
    char* config;
    char* fallback;
    if (given && *given) return copy_string(given);
    root = join(repo, name);
    if (is_file(root)) return root;
    home = user_home();
    config = join(home, ".switch");
    fallback = join(config, name);
    release(home);
    release(config);
    if (is_file(fallback)) {
        release(root);
        return fallback;
    }
    release(fallback);
    return root;
}

static int name_char(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static int hex_char(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/* In-place name = hex parser: ASCII names, optional whitespace. */
static int key_line(char* line, char** name, char** value) {
    char* p;
    char* name_end;
    char* value_end;
    p = line;
    while (isspace((unsigned char)*p)) ++p;
    *name = p;
    while (name_char((unsigned char)*p)) ++p;
    name_end = p;
    if (p == *name) return 0;
    while (isspace((unsigned char)*p)) ++p;
    if (*p++ != '=') return 0;
    while (isspace((unsigned char)*p)) ++p;
    *value = p;
    while (hex_char((unsigned char)*p)) ++p;
    value_end = p;
    if (p == *value) return 0;
    while (isspace((unsigned char)*p)) ++p;
    if (*p) return 0;
    *name_end = '\0';
    *value_end = '\0';
    return 1;
}

static unsigned long clean_keys(const char* source, const char* dest) {
    FILE* input;
    FILE* result;
    char* line;
    char* name;
    char* value;
    unsigned long dropped;
    input = open_file(source, "r");
    result = open_file(dest, "w");
    dropped = 0;
    while ((line = read_line(input)) != NULL) {
        if (key_line(line, &name, &value)) {
            if (strlen(value) % 32)
                ++dropped;
            else if (fprintf(result, "%s = %s\n", name, value) < 0)
                die("cannot write cleaned keys");
        }
        release(line);
    }
    close_file(input, source);
    close_file(result, dest);
    return dropped;
}

static void hex_string(const unsigned char* bytes, char* text) {
    static const char digits[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < 16; ++i) {
        text[i * 2] = digits[bytes[i] >> 4];
        text[i * 2 + 1] = digits[bytes[i] & 15];
    }
    text[32] = '\0';
}

static int ticket_title_key(const char* path, char* rights, char* key) {
    unsigned char data[0x3b0];
    unsigned long signature;
    size_t size, start;
    FILE* file;
    file = open_file(path, "rb");
    size = fread(data, 1, sizeof(data), file);
    close_file(file, path);
    if (size < 4) return 0;
    signature = (unsigned long)data[0] | ((unsigned long)data[1] << 8) | ((unsigned long)data[2] << 16) |
                ((unsigned long)data[3] << 24);
    switch (signature) {
        case 0x10000UL:
        case 0x10003UL: start = 4 + 0x23c; break;
        case 0x10001UL:
        case 0x10004UL: start = 4 + 0x13c; break;
        case 0x10002UL:
        case 0x10005UL: start = 4 + 0x7c; break;
        case 0x10006UL: start = 4 + 0x3c; break;
        default: return 0;
    }
    if (size < start + 0x170 || data[start + 0x141]) return 0;
    hex_string(data + start + 0x160, rights);
    hex_string(data + start + 0x40, key);
    return 1;
}

typedef struct {
    Strings names;
    Strings values;
} Keys;

static void add_key(Keys* keys, const char* name, const char* value) {
    char* lower;
    char* p;
    size_t i;
    lower = copy_string(name);
    for (p = lower; *p; ++p) *p = (char)tolower((unsigned char)*p);
    for (i = 0; i < keys->names.count; ++i) {
        if (!strcmp(keys->names.items[i], lower)) {
            release(lower);
            return;
        }
    }
    strings_add(&keys->names, lower);
    strings_add(&keys->values, value);
    release(lower);
}

static void ticket_keys(Keys* keys, const char* folder) {
    Strings files;
    char rights[33], key[33];
    char* path;
    size_t i;
    if (!folder) return;
    files = list_dir(folder, 1);
    for (i = 0; i < files.count; ++i) {
        if (!ends_with(files.items[i], ".tik")) continue;
        path = join(folder, files.items[i]);
        if (ticket_title_key(path, rights, key)) add_key(keys, rights, key);
        release(path);
    }
    strings_free(&files);
}

static void title_keys(const char* source, const char* dest, const char* base, const char* update) {
    Keys keys;
    FILE* file;
    char* line;
    char* name;
    char* value;
    size_t i;
    memset(&keys, 0, sizeof(keys));
    if (is_file(source)) {
        file = open_file(source, "r");
        while ((line = read_line(file)) != NULL) {
            if (key_line(line, &name, &value)) add_key(&keys, name, value);
            release(line);
        }
        close_file(file, source);
    }
    ticket_keys(&keys, base);
    ticket_keys(&keys, update);
    file = open_file(dest, "w");
    for (i = 0; i < keys.names.count; ++i) {
        if (fprintf(file, "%s = %s\n", keys.names.items[i], keys.values.items[i]) < 0) die("cannot write title keys");
    }
    close_file(file, dest);
    strings_free(&keys.names);
    strings_free(&keys.values);
}

static int mentions_key(const char* line, size_t length) {
    size_t i;
    for (i = 0; i + 2 < length; ++i) {
        if (tolower((unsigned char)line[i]) == 'k' && tolower((unsigned char)line[i + 1]) == 'e' &&
            tolower((unsigned char)line[i + 2]) == 'y')
            return 1;
    }
    return 0;
}

static char* hac_run(char* const* arguments) {
    Strings command;
    Process process;
    char* combined;
    char* filtered;
    char* p;
    char* start;
    size_t i, used, length;
    memset(&command, 0, sizeof(command));
    strings_add(&command, hactool);
    strings_add(&command, "--disablekeywarns");
    for (i = 0; arguments[i]; ++i) strings_add(&command, arguments[i]);
    process = run_process(command.items, NULL, 0, 1, isolated_home);
    strings_free(&command);
    if (process.status) {
        combined = concat(process.out, process.err);
        filtered = (char*)alloc(strlen(combined) + 1);
        used = 0;
        p = combined;
        while (*p) {
            start = p;
            while (*p && *p != '\n' && *p != '\r') ++p;
            length = (size_t)(p - start);
            if (!mentions_key(start, length)) {
                if (used) filtered[used++] = '\n';
                memcpy(filtered + used, start, length);
                used += length;
            }
            if (*p == '\r') ++p;
            if (*p == '\n') ++p;
        }
        filtered[used] = '\0';
        if (!used)
            die("hactool failed (exit %d): key-related output hidden; check keys match the dump", process.status);
        die("hactool failed (exit %d):\n%s", process.status, filtered + (used > 2000 ? used - 2000 : 0));
    }
    release(process.err);
    return process.out;
}

static void unpack(const char* container, const char* dest) {
    char* lower;
    char* p;
    char* args[4];
    char* info;
    lower = copy_string(container);
    for (p = lower; *p; ++p) *p = (char)tolower((unsigned char)*p);
    args[0] = ends_with(lower, ".xci") ? "-txci" : "-tpfs0";
    args[1] = concat(ends_with(lower, ".xci") ? "--securedir=" : "--outdir=", dest);
    args[2] = (char*)container;
    args[3] = NULL;
    printf("Unpack: %s\n", container);
    fflush(stdout);
    info = hac_run(args);
    release(info);
    release(args[1]);
    release(lower);
}

static int compare_strings(const void* a, const void* b) {
    return strcmp(*(const char* const*)a, *(const char* const*)b);
}

static char* program_nca(const char* folder, const char* container) {
    Strings files, kinds;
    char* best;
    char* path;
    char* info;
    char* kind;
    char* end;
    char* args[3];
    double size, best_size;
    size_t i, j;
    files = list_dir(folder, 1);
    memset(&kinds, 0, sizeof(kinds));
    best = NULL;
    best_size = 0;
    for (i = 0; i < files.count; ++i) {
        if (!ends_with(files.items[i], ".nca") || ends_with(files.items[i], ".cnmt.nca")) continue;
        path = join(folder, files.items[i]);
        args[0] = "-i";
        args[1] = path;
        args[2] = NULL;
        info = hac_run(args);
        kind = strstr(info, "Content Type:");
        if (kind) {
            kind += strlen("Content Type:");
            while (isspace((unsigned char)*kind)) ++kind;
            end = kind;
            while (name_char((unsigned char)*end)) ++end;
            *end = '\0';
            if (*kind) {
                for (j = 0; j < kinds.count; ++j)
                    if (!strcmp(kind, kinds.items[j])) break;
                if (j == kinds.count) strings_add(&kinds, kind);
                if (!strcmp(kind, "Program")) {
                    size = file_size(path);
                    if (!best || size > best_size) {
                        release(best);
                        best = copy_string(path);
                        best_size = size;
                    }
                }
            }
        }
        release(info);
        release(path);
    }
    strings_free(&files);
    if (!best) {
        if (kinds.count > 1) qsort(kinds.items, kinds.count, sizeof(char*), compare_strings);
        fprintf(stderr, "%s: no Program NCA (found: ", container);
        if (!kinds.count) fputs("none", stderr);
        for (i = 0; i < kinds.count; ++i) fprintf(stderr, "%s%s", i ? ", " : "", kinds.items[i]);
        die("); likely DLC. Use a base game; pass updates with --update.");
    }
    strings_free(&kinds);
    return best;
}

static char* base_stem(const char* path) {
    const char* p;
    const char* start;
    char* result;
    char* dot;
    start = path;
    for (p = path; *p; ++p) {
        if (*p == '/'
#ifdef _WIN32
            || *p == '\\'
#endif
        )
            start = p + 1;
    }
    result = copy_string(start);
    dot = strrchr(result, '.');
    /* Python splitext keeps leading dots (e.g. .game, ...game) intact. */
    if (dot) {
        for (p = result; p < dot && *p == '.'; ++p) {}
        if (p < dot) *dot = '\0';
    }
    return result;
}

int main(int argc, char** argv) {
    const char* base_input;
    const char* update_input;
    const char* name;
    const char* out_arg;
    const char* keys_arg;
    const char* titles_arg;
    const char* value;
    const char* equals;
    char** slot;
    char* parsed[6];
    const char* options[6];
    char* repo;
    char* keys;
    char* titles;
    char* config;
    char* parent;
    char* path;
    char* base_dir;
    char* update_dir;
    char* base;
    char* update;
    char* exefs;
    char* romfs;
    char* args[6];
    char* info;
    unsigned long dropped;
    double need;
    int i, j, positional;
    size_t length;

    host_arguments(&argc, &argv);
    base_input = NULL;
    positional = 0;
    memset(parsed, 0, sizeof(parsed));
    options[0] = "--update";
    options[1] = "--name";
    options[2] = "--out";
    options[3] = "--keys";
    options[4] = "--title-keys";
    options[5] = NULL;
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
            equals = strchr(argv[i], '=');
            length = equals ? (size_t)(equals - argv[i]) : strlen(argv[i]);
            slot = NULL;
            for (j = 0; options[j]; ++j) {
                if (strlen(options[j]) == length && !strncmp(options[j], argv[i], length)) slot = &parsed[j];
            }
            if (!slot) {
                fprintf(stderr, "unknown option: %s\n", argv[i]);
                return 2;
            }
            if (equals)
                value = equals + 1;
            else {
                if (i + 1 == argc || argv[i + 1][0] == '-') {
                    fprintf(stderr, "missing value: %s\n", argv[i]);
                    return 2;
                }
                value = argv[++i];
            }
            *slot = (char*)value;
        } else {
            if (base_input) {
                fputs("expected one base game\n", stderr);
                return 2;
            }
            base_input = argv[i];
        }
    }
    if (!base_input) {
        usage();
        return 2;
    }
    update_input = parsed[0];
    name = parsed[1];
    out_arg = parsed[2];
    keys_arg = parsed[3];
    titles_arg = parsed[4];
    if (update_input && !*update_input) update_input = NULL;
    if (!is_file(base_input)) die("not a file: %s", base_input);
    if (update_input && !is_file(update_input)) die("not a file: %s", update_input);
    repo = repo_path(argv[0]);
    keys = find_keys(keys_arg, "prod.keys", repo);
    titles = find_keys(titles_arg, "title.keys", repo);
    hactool = join(repo, "build/default/hactool" SNX_EXE);
    if (!is_file(hactool)) {
        puts("Build: hactool");
        args[0] = "cmake";
        args[1] = "--preset";
        args[2] = "default";
        args[3] = NULL;
        require_process(args, repo, 1);
        {
            char* build[8];
            build[0] = "cmake";
            build[1] = "--build";
            build[2] = "--preset";
            build[3] = "default";
            build[4] = "--target";
            build[5] = "hactool";
            build[6] = NULL;
            require_process(build, repo, 1);
        }
    }
    if (!is_file(keys)) {
        if (keys_arg && *keys_arg) die("not a file: %s", keys);
        die("prod.keys missing: %s or home/.switch/; use --keys FILE", repo);
    }
    if (!name || !*name) name = base_stem(base_input);
    if (out_arg && *out_arg)
        output = absolute_path(out_arg);
    else {
        parent = join(repo, "work");
        path = join(parent, name);
        output = absolute_path(path);
        release(path);
        release(parent);
    }
    exefs = join(output, "exefs");
    romfs = join(output, "romfs");
    if (path_exists(exefs)) die("exefs exists: %s; choose another --out", exefs);
    made_output = !path_exists(output);
    if (atexit(cleanup)) die("cannot register extraction cleanup");
    make_dirs(output);
    need = file_size(base_input);
    if (update_input) need += file_size(update_input);
    parent = temporary_parent(output, need * 1.5);
    temporary = temporary_dir(parent);
    release(parent);
    isolated_home = join(temporary, "home");
    config = join(isolated_home, ".switch");
    make_dirs(config);
    path = join(config, "prod.keys");
    dropped = clean_keys(keys, path);
    release(path);
    if (dropped) printf("Skipped %lu malformed key lines\n", dropped);
    base_dir = join(temporary, "base");
    unpack(base_input, base_dir);
    update_dir = NULL;
    if (update_input) {
        update_dir = join(temporary, "update");
        unpack(update_input, update_dir);
    }
    path = join(config, "title.keys");
    title_keys(titles, path, base_dir, update_dir);
    release(path);
    base = program_nca(base_dir, base_input);
    i = 0;
    if (update_dir) {
        update = program_nca(update_dir, update_input);
        puts("Extract: base + update");
        args[i++] = concat("--basenca=", base);
    } else {
        update = base;
        puts("Extract: base");
    }
    args[i++] = concat("--exefsdir=", exefs);
    args[i++] = concat("--romfsdir=", romfs);
    args[i++] = update;
    args[i] = NULL;
    info = hac_run(args);
    release(info);
    path = join(exefs, "main");
    if (!is_file(path)) die("exefs/main missing; check base/update compatibility");
    succeeded = 1;
    cleanup();
    printf("Done: %s\n", output);
    return 0;
}
