/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright 2026 the ScarletNX contributors
 * C90 OS layer: POSIX + Windows.
 */
#ifndef SNX_PORTABLE_H
#define SNX_PORTABLE_H

#include <stddef.h>
#include <stdio.h>

#ifdef _WIN32
#define SNX_EXE    ".exe"
#define SNX_TARGET "win64"
#define SNX_PYTHON "python"
#else
#define SNX_EXE    ""
#define SNX_TARGET "linux"
#define SNX_PYTHON "python3"
#endif

typedef struct {
    char** items;
    size_t count;
    size_t capacity;
} Strings;

typedef struct {
    int status;
    char* out;
    char* err;
} Process;

/* Strings: UTF-8 on Windows, native bytes on POSIX. */
void host_arguments(int* argc, char*** argv);
const char* environment(const char* name);
void die(const char* format, ...);
/* atexit reclaims allocations after app cleanup. */
void* alloc(size_t size);
void release(void* ptr);
char* copy_string(const char* text);
char* concat(const char* a, const char* b);
char* join(const char* a, const char* b);
char* parent_path(const char* path);
char* absolute_path(const char* path);
char* repo_path(const char* argv0);
char* user_home(void);
int ends_with(const char* text, const char* suffix);
int is_file(const char* path);
int is_dir(const char* path);
int path_exists(const char* path);
int is_link(const char* path);
double file_size(const char* path);
void make_dirs(const char* path);
void remove_tree(const char* path);
char* temporary_parent(const char* out, double need);
char* temporary_dir(const char* parent);
Strings list_dir(const char* path, int required);
void strings_add(Strings* list, const char* text);
void strings_free(Strings* list);
FILE* open_file(const char* path, const char* mode);
void close_file(FILE* file, const char* path);
char* read_stream(FILE* file);
char* read_line(FILE* file);
/* No shell. Nullable cwd/home; home isolates hactool config.
 * capture: separate stdout/stderr; quiet: discard stdout only. */
Process run_process(char* const* args, const char* cwd, int quiet, int capture, const char* home);
void process_free(Process* process);
void require_process(char* const* args, const char* cwd, int quiet);

#endif
