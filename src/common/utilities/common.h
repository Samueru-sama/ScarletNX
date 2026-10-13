/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_TOOL_COMMON_H
#define SNX_TOOL_COMMON_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* C90 implementation; fixed-width types come from the host SDK. */
typedef uint32_t SnxU32;
typedef uint64_t SnxU64;
typedef struct {
    const char* repo;
    const char* build;
    const char* executable;
} SnxPaths;

void* snx_alloc(size_t size);
void* snx_realloc(void* ptr, size_t size);
char* snx_strdup(const char* s);
char* snx_format(const char* fmt, ...);
int snx_error(const char* fmt, ...);
char* snx_path_join(const char* a, const char* b);
char* snx_dirname(const char* path);
char* snx_absolute(const char* path);
char* snx_executable(const char* argv0);
int snx_is_file(const char* path);
int snx_is_dir(const char* path);
int snx_is_link(const char* path);
int snx_mkdirs(const char* path);
int snx_remove_tree(const char* path);
int snx_copy_file(const char* src, const char* dst);
int snx_copy_tree(const char* src, const char* dst);
int snx_path_within(const char* path, const char* parent);
int snx_replace(const char* src, const char* dst);
int snx_list_dir(const char* path, char*** names, size_t* count);
void snx_free_strings(char** strings, size_t count);
unsigned char* snx_read_file(const char* path, size_t* size);
int snx_write_file(const char* path, const void* data, size_t size);
int snx_write_changed(const char* path, const void* data, size_t size);
char* snx_temp_dir(const char* parent);
SnxU64 snx_free_space(const char* path);
SnxU64 snx_file_size(const char* path);
int snx_make_executable(const char* path);
unsigned snx_cpu_count(void);
void snx_tool_console(void);
char* snx_find_tool(const char* name);
/* argv is NULL terminated. No shell. Captured output is heap-owned by caller.
   home isolates HOME/USERPROFILE and removes XDG_CONFIG_HOME in the child. */
int snx_run(const char* const* argv, const char* cwd, const char* home, char** output);
/* Foreground workflow stage with inherited stdio, interruption forwarding and diagnostics.
   A stopped POSIX child is aborted so a rerun can restart that stage. Not thread safe. */
int snx_run_stage(const char* const* argv, const char* cwd, const char* stage);
/* As above, but intentional debugger stops do not abort the game. */
int snx_run_debug_stage(const char* const* argv, const char* cwd, const char* stage);
FILE* snx_fopen(const char* path, const char* mode);

#endif
