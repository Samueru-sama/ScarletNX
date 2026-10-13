/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_GNU_VFS_INTERNAL_H
#define SNX_GNU_VFS_INTERNAL_H
#include "filesystem/vfs.h"
#include "abi/errno/errno.h"
#define GNU_VFS_PATH_MAX 4096
#define GNU_VFS_FD_MAX   256
#define GNU_VFS_FILE_MAX (64u * 1024u * 1024u)

enum GnuNodeKind { GNU_NODE_DIR, GNU_NODE_FILE, GNU_NODE_DEVICE, GNU_NODE_PROC, GNU_NODE_LINK, GNU_NODE_STDIO };
enum GnuDeviceKind { GNU_DEV_NULL, GNU_DEV_ZERO, GNU_DEV_RANDOM };
enum GnuProcKind { GNU_PROC_MAPS, GNU_PROC_CMDLINE, GNU_PROC_STATUS, GNU_PROC_CPUINFO };
enum {
    GNU_O_ACCMODE = 3,
    GNU_O_WRONLY = 1,
    GNU_O_RDWR = 2,
    GNU_O_CREAT = 64,
    GNU_O_EXCL = 128,
    GNU_O_TRUNC = 512,
    GNU_O_APPEND = 1024,
    GNU_O_NONBLOCK = 2048,
    GNU_O_DIRECTORY = 16384,
    GNU_O_NOFOLLOW = 32768,
    GNU_O_CLOEXEC = 524288,
    GNU_O_PATH = 2097152,
    GNU_AT_FDCWD = -100,
    GNU_AT_SYMLINK_NOFOLLOW = 256,
    GNU_AT_REMOVEDIR = 512,
    GNU_AT_EMPTY_PATH = 4096
};
typedef struct GnuNode {
    struct GnuNode *parent, *children, *sibling, *allocated_next;
    char* name;
    enum GnuNodeKind kind;
    unsigned mode;
    unsigned subtype;
    uint64_t inode;
    unsigned char* bytes;
    size_t size, capacity;
    int readonly, deleted, host_fd;
} GnuNode;
typedef struct GnuOpen {
    GnuNode* node;
    uint64_t offset;
    unsigned flags, references;
    int fdlink; /* O_PATH|O_NOFOLLOW descriptor for a /dev/fd or proc fd symlink. */
    unsigned char* snapshot;
    size_t snapshot_size;
} GnuOpen;
typedef struct GnuDescriptor {
    GnuOpen* file;
    unsigned flags;
} GnuDescriptor;
typedef struct GnuVfsState {
    GnuNode *root, *allocated, *cwd, *executable;
    GnuDescriptor descriptors[GNU_VFS_FD_MAX];
    uint64_t next_inode;
    uint32_t pid;
    char* executable_path;
    unsigned char* cmdline;
    size_t cmdline_size;
} GnuVfsState;

GnuNode* gnu_vfs_node(GnuVfsState* state, GnuNode* parent, const char* name, enum GnuNodeKind kind, unsigned mode,
                      int readonly);
int gnu_devfs_init(GnuVfsState* state);
int64_t gnu_devfs_read(unsigned kind, void* bytes, size_t length);
int gnu_tmpfs_resize(GnuNode* node, size_t size);
int gnu_procfs_init(GnuVfsState* state);
int gnu_procfs_snapshot(GnuVfsState* state, GnuMemory* memory, unsigned kind, unsigned char** bytes, size_t* length);
void gnu_vfs_put16(unsigned char* bytes, uint16_t value);
void gnu_vfs_put32(unsigned char* bytes, uint32_t value);
void gnu_vfs_put64(unsigned char* bytes, uint64_t value);
#endif
