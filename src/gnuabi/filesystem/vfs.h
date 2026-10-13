/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_GNU_VFS_H
#define SNX_GNU_VFS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "memory/memory.h"

typedef struct GnuVfs {
    struct GnuVfsState* state;
} GnuVfs;
int gnu_vfs_init(GnuVfs* vfs);
void gnu_vfs_destroy(GnuVfs* vfs);
int gnu_vfs_set_process(GnuVfs* vfs, uint32_t pid, const char* exe, int argc, const char* const* argv);
/* The only imported host file is the executable already validated by the ELF loader. */
int gnu_vfs_set_executable(GnuVfs* vfs, const void* bytes, size_t length);
bool gnu_vfs_syscall(GnuVfs* vfs, GnuMemory* memory, uint64_t number, const uint64_t args[6], int64_t* result);
int64_t gnu_vfs_pread(GnuVfs* vfs, int fd, void* destination, size_t length, uint64_t offset);
#endif
