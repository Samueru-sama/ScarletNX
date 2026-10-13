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

/* Descriptors backed by another subsystem's object (network/: sockets, eventfd, epoll). read(), write(), close(),
   dup(), fcntl() and fstat() reach them through the descriptor table like files. The calls get the open file's
   flags, so O_NONBLOCK set with fcntl() or FIONBIO applies to the next call. */
typedef struct GnuObjectOps {
    const char* link;   /* readlink() of /proc/self/fd/N: "socket" gives "socket:[inode]" */
    unsigned file_type; /* st_mode's type bits: 0140000 for a socket, 0 for an anonymous inode */
    int64_t (*read)(void* object, unsigned flags, void* data, size_t size);
    int64_t (*write)(void* object, unsigned flags, const void* data, size_t size);
    int (*readable_bytes)(void* object, int* bytes); /* FIONREAD, NULL when it has none */
    void (*release)(void* object);                   /* the last descriptor is closed */
} GnuObjectOps;
/* installs a descriptor at the lowest free number, or -GNU_EMFILE (then the object is released) */
int gnu_vfs_install_object(GnuVfs* vfs, const GnuObjectOps* ops, void* object, unsigned flags, int cloexec);
/* the object behind fd when it is of kind ops, and the open file's flags; NULL otherwise */
void* gnu_vfs_object(GnuVfs* vfs, int fd, const GnuObjectOps* ops, unsigned* flags);
/* what poll() needs to know about a descriptor */
typedef struct GnuFdInfo {
    const GnuObjectOps* ops; /* an object's kind, or NULL */
    void* object;
    int host_fd; /* the host descriptor of stdio, -1 for the others */
    int regular; /* a file, directory or device: always ready */
    unsigned flags;
    const void* identity; /* the open file, which tells a reused descriptor number from the one registered */
} GnuFdInfo;
int gnu_vfs_fd_info(GnuVfs* vfs, int fd, GnuFdInfo* out); /* 0 or -GNU_EBADF */
/* a read-only file the runtime provides (/etc/resolv.conf), its directories made as needed */
int gnu_vfs_add_file(GnuVfs* vfs, const char* path, const void* data, size_t size);
#endif
