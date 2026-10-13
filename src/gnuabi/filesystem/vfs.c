/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Guest filesystem only: paths never reach host open/stat/unlink APIs.
   ABI constants/layout follow Linux asm-generic UAPI; host errors are translated. */
#include "filesystem/internal.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#define host_read  _read
#define host_write _write
#else
#include <unistd.h>
#define host_read  read
#define host_write write
#endif

void gnu_vfs_put16(unsigned char* p, uint16_t v) {
    for (unsigned i = 0; i < 2; ++i) p[i] = (unsigned char)(v >> (i * 8));
}
void gnu_vfs_put32(unsigned char* p, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) p[i] = (unsigned char)(v >> (i * 8));
}
void gnu_vfs_put64(unsigned char* p, uint64_t v) {
    for (unsigned i = 0; i < 8; ++i) p[i] = (unsigned char)(v >> (i * 8));
}
static uint64_t get64(const unsigned char* p) {
    uint64_t v = 0;
    for (unsigned i = 0; i < 8; ++i) v |= (uint64_t)p[i] << (i * 8);
    return v;
}
static char* copy_string(const char* source) {
    size_t size = strlen(source) + 1;
    char* copy = (char*)malloc(size);
    if (copy) memcpy(copy, source, size);
    return copy;
}
static int host_errno(void) {
    /* errno numbering differs on Windows; never return -errno directly. */
    switch (errno) {
        case EBADF: return -GNU_EBADF;
        case EINVAL: return -GNU_EINVAL;
        case EINTR: return -GNU_EINTR;
        case EAGAIN: return -GNU_EAGAIN;
        case ENOMEM: return -GNU_ENOMEM;
        case EACCES: return -GNU_EACCES;
        case ENOSPC: return -GNU_ENOSPC;
        case EPIPE: return -GNU_EPIPE;
        default: return -GNU_EIO;
    }
}
GnuNode* gnu_vfs_node(GnuVfsState* s, GnuNode* parent, const char* name, enum GnuNodeKind kind, unsigned mode,
                      int readonly) {
    GnuNode* node = (GnuNode*)calloc(1, sizeof *node);
    if (!node) return NULL;
    node->name = copy_string(name);
    if (!node->name) {
        free(node);
        return NULL;
    }
    node->parent = parent;
    node->kind = kind;
    node->mode = mode;
    node->readonly = readonly;
    node->inode = ++s->next_inode;
    node->allocated_next = s->allocated;
    s->allocated = node;
    if (parent) {
        GnuNode** next = &parent->children;
        while (*next) next = &(*next)->sibling;
        *next = node;
    }
    return node;
}
static GnuDescriptor* descriptor(GnuVfsState* s, int fd) {
    if (fd < 0 || fd >= GNU_VFS_FD_MAX || !s->descriptors[fd].file) return NULL;
    return &s->descriptors[fd];
}
static void drop_descriptor(GnuDescriptor* d) {
    if (d->file && --d->file->references == 0) {
        free(d->file->snapshot);
        free(d->file);
    }
    d->file = NULL;
    d->flags = 0;
}
static int install(GnuVfsState* s, GnuOpen* file, unsigned flags, int minimum) {
    if (minimum < 0 || minimum >= GNU_VFS_FD_MAX) return -GNU_EINVAL;
    for (int fd = minimum; fd < GNU_VFS_FD_MAX; ++fd)
        if (!s->descriptors[fd].file) {
            s->descriptors[fd].file = file;
            s->descriptors[fd].flags = flags & 1;
            ++file->references;
            return fd;
        }
    return -GNU_EMFILE;
}
static int node_path(GnuNode* node, char* destination, size_t capacity) {
    const GnuNode* chain[GNU_VFS_PATH_MAX / 2];
    size_t count = 0, size = 1;
    while (node && node->parent) {
        if (count >= sizeof chain / sizeof chain[0]) return -GNU_ENAMETOOLONG;
        chain[count++] = node;
        size += strlen(node->name) + 1;
        node = node->parent;
    }
    if (size > capacity) return -GNU_ENAMETOOLONG;
    char* p = destination;
    *p++ = '/';
    while (count) {
        const char* name = chain[--count]->name;
        size_t n = strlen(name);
        memcpy(p, name, n);
        p += n;
        if (count) *p++ = '/';
    }
    *p = 0;
    return 0;
}
static int fd_directory(const GnuNode* n) {
    return n && n->parent && !strcmp(n->name, "fd") &&
           (!strcmp(n->parent->name, "dev") || !strcmp(n->parent->name, "self"));
}
static GnuNode* child(GnuNode* parent, const char* name) {
    for (GnuNode* n = parent->children; n; n = n->sibling)
        if (!n->deleted && !strcmp(n->name, name)) return n;
    return NULL;
}
static int resolve(GnuVfsState* s, int dirfd, const char* path, int follow, GnuNode** out, int* fdlink) {
    size_t length = strlen(path);
    if (!length) return -GNU_ENOENT;
    if (length >= GNU_VFS_PATH_MAX) return -GNU_ENAMETOOLONG;
    GnuNode* node = s->root;
    if (path[0] != '/') {
        if (dirfd == GNU_AT_FDCWD)
            node = s->cwd;
        else {
            GnuDescriptor* d = descriptor(s, dirfd);
            if (!d) return -GNU_EBADF;
            node = d->file->node;
        }
        if (node->kind != GNU_NODE_DIR) return -GNU_ENOTDIR;
        if (!(node->mode & 0100)) return -GNU_EACCES;
    }
    if (fdlink) *fdlink = -1;
    const char* p = path;
    while (*p) {
        while (*p == '/') ++p;
        if (!*p) break;
        if (node->kind != GNU_NODE_DIR) return -GNU_ENOTDIR;
        if (!(node->mode & 0100)) return -GNU_EACCES;
        const char* start = p;
        while (*p && *p != '/') ++p;
        size_t size = (size_t)(p - start);
        if (size > 255) return -GNU_ENAMETOOLONG;
        char name[256];
        memcpy(name, start, size);
        name[size] = 0;
        const char* after = p;
        while (*after == '/') ++after;
        int final = !*after;
        if (!strcmp(name, ".")) continue;
        if (!strcmp(name, "..")) {
            if (node->parent) node = node->parent;
            continue;
        }
        if (fd_directory(node)) {
            char* end = NULL;
            long fd = strtol(name, &end, 10);
            if (!*name || *end || fd < 0 || fd >= GNU_VFS_FD_MAX || !descriptor(s, (int)fd)) return -GNU_ENOENT;
            node = s->descriptors[fd].file->node;
            if (final && fdlink) *fdlink = (int)fd;
        } else {
            /* /proc/<guest pid> is equivalent to /proc/self. */
            if (node->parent == s->root && !strcmp(node->name, "proc")) {
                char pid[32];
                snprintf(pid, sizeof pid, "%u", s->pid);
                if (!strcmp(name, pid)) strcpy(name, "self");
            }
            node = child(node, name);
            if (!node) return -GNU_ENOENT;
            if (node->kind == GNU_NODE_LINK && (!final || follow)) {
                if (!s->executable) return -GNU_ENOENT;
                node = s->executable;
            }
        }
        if (final && *p == '/' && node->kind != GNU_NODE_DIR) return -GNU_ENOTDIR;
    }
    *out = node;
    return 0;
}
static int parent_path(GnuVfsState* s, int dirfd, const char* path, GnuNode** parent, char name[256]) {
    size_t size = strlen(path);
    if (!size) return -GNU_ENOENT;
    if (size >= GNU_VFS_PATH_MAX) return -GNU_ENAMETOOLONG;
    while (size > 1 && path[size - 1] == '/') --size;
    size_t first = size;
    while (first && path[first - 1] != '/') --first;
    size_t n = size - first;
    if (!n || n > 255) return n ? -GNU_ENAMETOOLONG : -GNU_EINVAL;
    memcpy(name, path + first, n);
    name[n] = 0;
    if (!strcmp(name, ".") || !strcmp(name, "..")) return -GNU_EINVAL;
    char prefix[GNU_VFS_PATH_MAX];
    if (!first)
        strcpy(prefix, ".");
    else {
        memcpy(prefix, path, first);
        prefix[first] = 0;
    }
    int error = resolve(s, dirfd, prefix, 1, parent, NULL);
    if (error) return error;
    return (*parent)->kind == GNU_NODE_DIR ? 0 : -GNU_ENOTDIR;
}
int gnu_vfs_init(GnuVfs* vfs) {
    vfs->state = (GnuVfsState*)calloc(1, sizeof *vfs->state);
    if (!vfs->state) return -GNU_ENOMEM;
    GnuVfsState* s = vfs->state;
    s->pid = 1000;
    s->root = gnu_vfs_node(s, NULL, "", GNU_NODE_DIR, 0755, 1);
    s->cwd = s->root;
    if (!s->root || !gnu_vfs_node(s, s->root, "tmp", GNU_NODE_DIR, 01777, 0) || gnu_devfs_init(s) ||
        gnu_procfs_init(s)) {
        gnu_vfs_destroy(vfs);
        return -GNU_ENOMEM;
    }
    for (int fd = 0; fd < 3; ++fd) {
        char name[32];
        snprintf(name, sizeof name, "stdio-%d", fd);
        GnuNode* n = gnu_vfs_node(s, NULL, name, GNU_NODE_STDIO, 0666, 0);
        GnuOpen* f = (GnuOpen*)calloc(1, sizeof *f);
        if (!n || !f) {
            free(f);
            gnu_vfs_destroy(vfs);
            return -GNU_ENOMEM;
        }
        n->host_fd = fd;
        f->node = n;
        f->flags = fd ? GNU_O_WRONLY : 0;
        f->fdlink = -1;
        install(s, f, 0, fd);
#if defined(_WIN32)
        _setmode(fd, _O_BINARY);
#endif
    }
    return 0;
}
void gnu_vfs_destroy(GnuVfs* vfs) {
    GnuVfsState* s = vfs->state;
    if (!s) return;
    for (int fd = 0; fd < GNU_VFS_FD_MAX; ++fd) drop_descriptor(&s->descriptors[fd]);
    GnuNode* n = s->allocated;
    while (n) {
        GnuNode* next = n->allocated_next;
        free(n->name);
        free(n->bytes);
        free(n);
        n = next;
    }
    free(s->cmdline);
    free(s->executable_path);
    free(s);
    vfs->state = NULL;
}
int gnu_vfs_set_process(GnuVfs* vfs, uint32_t pid, const char* exe, int argc, const char* const* argv) {
    if (!vfs->state || !exe || argc < 0) return -GNU_EINVAL;
    if (exe[0] != '/' || strlen(exe) >= GNU_VFS_PATH_MAX) return -GNU_EINVAL;
    size_t length = 0;
    for (int i = 0; i < argc; ++i) {
        size_t n = strlen(argv[i]) + 1;
        if (n > GNU_VFS_FILE_MAX - length) return -GNU_E2BIG;
        length += n;
    }
    unsigned char* cmdline = (unsigned char*)malloc(length ? length : 1);
    char* path = copy_string(exe);
    if (!cmdline || !path) {
        free(cmdline);
        free(path);
        return -GNU_ENOMEM;
    }
    size_t offset = 0;
    for (int i = 0; i < argc; ++i) {
        size_t n = strlen(argv[i]) + 1;
        memcpy(cmdline + offset, argv[i], n);
        offset += n;
    }
    GnuVfsState* s = vfs->state;
    free(s->cmdline);
    free(s->executable_path);
    s->cmdline = cmdline;
    s->cmdline_size = length;
    s->executable_path = path;
    s->pid = pid;
    return 0;
}
int gnu_vfs_set_executable(GnuVfs* vfs, const void* bytes, size_t length) {
    GnuVfsState* s = vfs->state;
    if (!s) return -GNU_EINVAL;
    if (!s->executable) {
        /* Import at a guest path so readlink(/proc/self/exe) resolves in this namespace. */
        const char* p = s->executable_path ? s->executable_path : "/program";
        GnuNode* parent = s->root;
        for (;;) {
            while (*p == '/') ++p;
            const char* start = p;
            while (*p && *p != '/') ++p;
            size_t size = (size_t)(p - start);
            if (!size || size > 255) return -GNU_EINVAL;
            char name[256];
            memcpy(name, start, size);
            name[size] = 0;
            if (!strcmp(name, ".") || !strcmp(name, "..")) return -GNU_EINVAL;
            while (*p == '/') ++p;
            int final = !*p;
            GnuNode* node = child(parent, name);
            if (!node) node = gnu_vfs_node(s, parent, name, final ? GNU_NODE_FILE : GNU_NODE_DIR, 0555, 1);
            if (!node) return -GNU_ENOMEM;
            if (final) {
                if (node->kind != GNU_NODE_FILE) return -GNU_EISDIR;
                node->readonly = 1;
                node->mode = 0555;
                s->executable = node;
                break;
            }
            if (node->kind != GNU_NODE_DIR) return -GNU_ENOTDIR;
            parent = node;
        }
    }
    int error = gnu_tmpfs_resize(s->executable, length);
    if (error) return error;
    if (length) memcpy(s->executable->bytes, bytes, length);
    return 0;
}
static int open_file(GnuVfsState* s, GnuMemory* memory, int dirfd, const char* path, unsigned flags, unsigned mode) {
    if (flags & GNU_O_PATH) flags &= GNU_O_PATH | GNU_O_DIRECTORY | GNU_O_NOFOLLOW | GNU_O_CLOEXEC;
    if ((flags & GNU_O_ACCMODE) == GNU_O_ACCMODE) return -GNU_EINVAL;
    if (flags & 4194304u) return -GNU_ENOTSUP; /* O_TMPFILE is not implemented. */
    int available = 0;
    for (int fd = 0; fd < GNU_VFS_FD_MAX; ++fd)
        if (!s->descriptors[fd].file) {
            available = 1;
            break;
        }
    if (!available) return -GNU_EMFILE;
    GnuNode* node = NULL;
    int fdlink = -1;
    int error = resolve(s, dirfd, path, !(flags & GNU_O_NOFOLLOW), &node, &fdlink);
    if (error == -GNU_ENOENT && (flags & GNU_O_CREAT)) {
        size_t length = strlen(path);
        if (length && path[length - 1] == '/') return -GNU_EISDIR;
        GnuNode* parent;
        char name[256];
        error = parent_path(s, dirfd, path, &parent, name);
        if (error) return error;
        if (parent->deleted) return -GNU_ENOENT;
        if (parent->readonly) return -GNU_EROFS;
        if (!(parent->mode & 0200)) return -GNU_EACCES;
        node = gnu_vfs_node(s, parent, name, GNU_NODE_FILE, mode & 0777u, 0);
        if (!node) return -GNU_ENOMEM;
    } else if (error)
        return error;
    else if ((flags & (GNU_O_CREAT | GNU_O_EXCL)) == (GNU_O_CREAT | GNU_O_EXCL))
        return -GNU_EEXIST;
    if (node->kind == GNU_NODE_LINK && !(flags & GNU_O_PATH)) return -GNU_ELOOP;
    if (fdlink >= 0 && (flags & GNU_O_NOFOLLOW) && !(flags & GNU_O_PATH)) return -GNU_ELOOP;
    if ((flags & GNU_O_DIRECTORY) && node->kind != GNU_NODE_DIR) return -GNU_ENOTDIR;
    if (node->kind == GNU_NODE_DIR && ((flags & GNU_O_ACCMODE) || (flags & GNU_O_TRUNC))) return -GNU_EISDIR;
    if (node->readonly && (flags & GNU_O_ACCMODE) && !(flags & GNU_O_PATH)) return -GNU_EACCES;
    if (!(flags & GNU_O_PATH)) {
        unsigned access = flags & GNU_O_ACCMODE;
        if (access != GNU_O_WRONLY && !(node->mode & 0400)) return -GNU_EACCES;
        if (access != 0 && !(node->mode & 0200)) return -GNU_EACCES;
    }
    if ((flags & GNU_O_TRUNC) && !(flags & GNU_O_PATH) && node->kind == GNU_NODE_FILE) {
        if (node->readonly) return -GNU_EACCES;
        error = gnu_tmpfs_resize(node, 0);
        if (error) return error;
    }
    GnuOpen* file = (GnuOpen*)calloc(1, sizeof *file);
    if (!file) return -GNU_ENOMEM;
    file->node = node;
    file->flags = flags & ~(GNU_O_CREAT | GNU_O_EXCL | GNU_O_TRUNC | GNU_O_CLOEXEC);
    file->fdlink = (flags & GNU_O_PATH) && (flags & GNU_O_NOFOLLOW) ? fdlink : -1;
    if (node->kind == GNU_NODE_PROC && !(flags & GNU_O_PATH)) {
        error = gnu_procfs_snapshot(s, memory, node->subtype, &file->snapshot, &file->snapshot_size);
        if (error) {
            free(file);
            return error;
        }
    }
    int fd = install(s, file, (flags & GNU_O_CLOEXEC) ? 1 : 0, 0);
    if (fd < 0) {
        free(file->snapshot);
        free(file);
    }
    return fd;
}
static int64_t file_read(GnuOpen* file, void* data, size_t length, uint64_t position) {
    GnuNode* node = file->node;
    if ((file->flags & GNU_O_PATH) || (file->flags & GNU_O_ACCMODE) == GNU_O_WRONLY) return -GNU_EBADF;
    if (node->kind == GNU_NODE_DIR) return -GNU_EISDIR;
    if (node->kind == GNU_NODE_DEVICE) return gnu_devfs_read(node->subtype, data, length);
    if (node->kind == GNU_NODE_STDIO) {
        size_t chunk = length > INT_MAX ? INT_MAX : length;
        int64_t count = host_read(node->host_fd, data, (unsigned)chunk);
        return count < 0 ? host_errno() : count;
    }
    const unsigned char* bytes = node->kind == GNU_NODE_PROC ? file->snapshot : node->bytes;
    size_t size = node->kind == GNU_NODE_PROC ? file->snapshot_size : node->size;
    if (position >= size) return 0;
    if (length > size - (size_t)position) length = size - (size_t)position;
    if (length) memcpy(data, bytes + (size_t)position, length);
    return (int64_t)length;
}
int64_t gnu_vfs_pread(GnuVfs* vfs, int fd, void* destination, size_t length, uint64_t offset) {
    GnuDescriptor* d = descriptor(vfs->state, fd);
    if (!d) return -GNU_EBADF;
    if (d->file->node->kind == GNU_NODE_STDIO) return -GNU_ESPIPE;
    return file_read(d->file, destination, length, offset);
}
static int64_t file_write(GnuOpen* file, const void* data, size_t length, uint64_t position, int positional) {
    GnuNode* node = file->node;
    if ((file->flags & GNU_O_PATH) || !(file->flags & GNU_O_ACCMODE)) return -GNU_EBADF;
    if (node->kind == GNU_NODE_DIR) return -GNU_EISDIR;
    if (node->kind == GNU_NODE_DEVICE) return (int64_t)length;
    if (node->kind == GNU_NODE_STDIO) {
        size_t chunk = length > INT_MAX ? INT_MAX : length;
        int64_t count = host_write(node->host_fd, data, (unsigned)chunk);
        return count < 0 ? host_errno() : count;
    }
    if (node->readonly) return -GNU_EACCES;
    if (file->flags & GNU_O_APPEND) position = node->size;
    if (!length) return 0;
    if (position > GNU_VFS_FILE_MAX || length > GNU_VFS_FILE_MAX - position) return -GNU_EFBIG;
    size_t end = (size_t)position + length;
    if (end > node->size) {
        int error = gnu_tmpfs_resize(node, end);
        if (error) return error;
    }
    memcpy(node->bytes + (size_t)position, data, length);
    if (!positional) file->offset = position + length;
    return (int64_t)length;
}
static int64_t io(GnuVfsState* s, GnuMemory* memory, int fd, uint64_t address, uint64_t length, int write,
                  int positional, uint64_t position) {
    GnuDescriptor* d = descriptor(s, fd);
    if (!d) return -GNU_EBADF;
    if (length > INT64_MAX || length > SIZE_MAX) return -GNU_EINVAL;
    if (positional && d->file->node->kind == GNU_NODE_STDIO) return -GNU_ESPIPE;
    if (positional && (int64_t)position < 0) return -GNU_EINVAL;
    unsigned char dummy = 0;
    /* Linux null/zero discard writes, and null reads do not touch the buffer. */
    if (d->file->node->kind == GNU_NODE_DEVICE && (write || d->file->node->subtype == GNU_DEV_NULL))
        return write ? file_write(d->file, &dummy, (size_t)length, 0, 0)
                     : file_read(d->file, &dummy, (size_t)length, 0);
    void* data =
        length ? gnu_memory_access(memory, address, (size_t)length, write ? GNU_PROT_READ : GNU_PROT_WRITE) : &dummy;
    if (!data) return -GNU_EFAULT;
    uint64_t offset = positional ? position : d->file->offset;
    int64_t result = write ? file_write(d->file, data, (size_t)length, offset, positional)
                           : file_read(d->file, data, (size_t)length, offset);
    if (result > 0 && !positional && !write && d->file->node->kind != GNU_NODE_DEVICE &&
        d->file->node->kind != GNU_NODE_STDIO)
        d->file->offset += (uint64_t)result;
    return result;
}
static int64_t vectored_io(GnuVfsState* s, GnuMemory* memory, int fd, uint64_t vectors, uint64_t count, int write) {
    if (!descriptor(s, fd)) return -GNU_EBADF;
    if (count > 1024) return -GNU_EINVAL;
    unsigned char entries[1024 * 16];
    int error = gnu_memory_read(memory, vectors, entries, (size_t)count * 16);
    if (error) return error;
    uint64_t total_length = 0;
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t n = get64(entries + (size_t)i * 16 + 8);
        if (n > INT64_MAX - total_length) return -GNU_EINVAL;
        total_length += n;
    }
    int64_t total = 0;
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t address = get64(entries + (size_t)i * 16), length = get64(entries + (size_t)i * 16 + 8);
        int64_t result = io(s, memory, fd, address, length, write, 0, 0);
        if (result < 0) return total ? total : result;
        total += result;
        if ((uint64_t)result < length) break;
    }
    return total;
}
static int64_t seek(GnuVfsState* s, int fd, int64_t offset, unsigned whence) {
    GnuDescriptor* d = descriptor(s, fd);
    if (!d) return -GNU_EBADF;
    GnuOpen* f = d->file;
    GnuNode* n = f->node;
    if (f->flags & GNU_O_PATH) return -GNU_EBADF;
    if (n->kind == GNU_NODE_STDIO) return -GNU_ESPIPE;
    if (whence > 2) return -GNU_EINVAL;
    if (n->kind == GNU_NODE_DEVICE) return 0;
    uint64_t base = whence == 0 ? 0 : whence == 1 ? f->offset : n->kind == GNU_NODE_PROC ? f->snapshot_size : n->size;
    uint64_t magnitude = offset < 0 ? (uint64_t)(-(offset + 1)) + 1 : (uint64_t)offset;
    if ((offset < 0 && magnitude > base) || (offset >= 0 && magnitude > INT64_MAX - base)) return -GNU_EINVAL;
    f->offset = offset < 0 ? base - magnitude : base + magnitude;
    return (int64_t)f->offset;
}
static unsigned node_type(const GnuNode* n) {
    switch (n->kind) {
        case GNU_NODE_DIR: return 0040000;
        case GNU_NODE_LINK: return 0120000;
        case GNU_NODE_DEVICE:
        case GNU_NODE_STDIO: return 0020000;
        default: return 0100000;
    }
}
static int stat_write(GnuVfsState* s, GnuMemory* memory, GnuNode* node, uint64_t address, int fdlink) {
    unsigned char bytes[128] = {0};
    uint64_t size = node->size;
    unsigned type = node_type(node);
    if (fdlink >= 0) type = 0120000;
    if (type == 0120000) {
        if (fdlink >= 0 && node != s->executable) {
            char target[GNU_VFS_PATH_MAX];
            if (node->kind == GNU_NODE_STDIO)
                snprintf(target, sizeof target, "pipe:[%d]", 10000 + node->host_fd);
            else {
                int error = node_path(node, target, sizeof target);
                if (error) return error;
            }
            size = strlen(target) + (node->deleted ? 10 : 0);
        } else
            size = s->executable_path ? strlen(s->executable_path) : 0;
    }
    if (type == 0040000) size = 4096;
    gnu_vfs_put64(bytes, 1);
    gnu_vfs_put64(bytes + 8, node->inode);
    gnu_vfs_put32(bytes + 16, type | (fdlink >= 0 ? 0777 : node->mode));
    gnu_vfs_put32(bytes + 20, node->deleted ? 0 : type == 0040000 ? 2 : 1);
    gnu_vfs_put32(bytes + 24, 1000);
    gnu_vfs_put32(bytes + 28, 1000);
    if (type == 0020000) {
        unsigned minor = node->kind != GNU_NODE_DEVICE   ? 3
                         : node->subtype == GNU_DEV_NULL ? 3
                         : node->subtype == GNU_DEV_ZERO ? 5
                         : !strcmp(node->name, "random") ? 8
                                                         : 9;
        gnu_vfs_put64(bytes + 32, 0x100u + minor);
    }
    gnu_vfs_put64(bytes + 48, size);
    gnu_vfs_put32(bytes + 56, 4096);
    gnu_vfs_put64(bytes + 64, (size + 511) / 512);
    /* Fixed deterministic guest times, with exact 128-byte asm-generic stat layout. */
    return gnu_memory_write(memory, address, bytes, sizeof bytes);
}
static int readlink_path(GnuVfsState* s, int dirfd, const char* path, char output[GNU_VFS_PATH_MAX]) {
    GnuNode* node = NULL;
    int fdlink = -1;
    int error = 0;
    if (!*path) {
        GnuDescriptor* d = descriptor(s, dirfd);
        if (!d) return -GNU_EBADF;
        node = d->file->node;
        fdlink = d->file->fdlink;
    } else
        error = resolve(s, dirfd, path, 0, &node, &fdlink);
    if (error) return error;
    if (fdlink >= 0) {
        if (node == s->executable) {
            if (!s->executable_path) return -GNU_ENOENT;
            strncpy(output, s->executable_path, GNU_VFS_PATH_MAX - 1);
            output[GNU_VFS_PATH_MAX - 1] = 0;
            return 0;
        }
        if (node->kind == GNU_NODE_STDIO) {
            snprintf(output, GNU_VFS_PATH_MAX, "pipe:[%d]", 10000 + node->host_fd);
            return 0;
        }
        error = node_path(node, output, GNU_VFS_PATH_MAX);
        if (error) return error;
        if (node->deleted) {
            size_t n = strlen(output);
            if (n + 11 >= GNU_VFS_PATH_MAX) return -GNU_ENAMETOOLONG;
            strcat(output, " (deleted)");
        }
        return 0;
    }
    if (node->kind != GNU_NODE_LINK) return -GNU_EINVAL;
    if (!s->executable_path) return -GNU_ENOENT;
    if (strlen(s->executable_path) >= GNU_VFS_PATH_MAX) return -GNU_ENAMETOOLONG;
    strcpy(output, s->executable_path);
    return 0;
}
static int64_t directory_entries(GnuVfsState* s, GnuMemory* memory, int fd, uint64_t address, uint64_t count) {
    GnuDescriptor* d = descriptor(s, fd);
    if (!d) return -GNU_EBADF;
    GnuOpen* f = d->file;
    GnuNode* dir = f->node;
    if (f->flags & GNU_O_PATH) return -GNU_EBADF;
    if (dir->kind != GNU_NODE_DIR) return -GNU_ENOTDIR;
    if (count > INT_MAX) count = INT_MAX;
    unsigned char* destination = (unsigned char*)gnu_memory_access(memory, address, (size_t)count, GNU_PROT_WRITE);
    if (count && !destination) return -GNU_EFAULT;
    size_t used = 0;
    uint64_t ordinal = 0;
    for (unsigned index = 0;; ++index) {
        const char* name = NULL;
        GnuNode* node = NULL;
        char number[32];
        int is_link = 0;
        if (index == 0) {
            name = ".";
            node = dir;
        } else if (index == 1) {
            name = "..";
            node = dir->parent ? dir->parent : dir;
        } else if (fd_directory(dir)) {
            unsigned target = index - 2;
            if (target >= GNU_VFS_FD_MAX) break;
            if (!s->descriptors[target].file) continue;
            snprintf(number, sizeof number, "%u", target);
            name = number;
            node = s->descriptors[target].file->node;
            is_link = 1;
        } else {
            unsigned which = index - 2;
            node = dir->children;
            while (node) {
                if (!node->deleted) {
                    if (!which) break;
                    --which;
                }
                node = node->sibling;
            }
            if (!node) break;
            name = node->name;
        }
        if (ordinal++ < f->offset) continue;
        size_t length = (19 + strlen(name) + 1 + 7) & ~(size_t)7;
        if (length > count - used) {
            if (!used) return -GNU_EINVAL;
            break;
        }
        unsigned char* record = destination + used;
        memset(record, 0, length);
        gnu_vfs_put64(record, node->inode);
        gnu_vfs_put64(record + 8, ordinal);
        gnu_vfs_put16(record + 16, (uint16_t)length);
        unsigned type = is_link ? 0120000 : node_type(node);
        record[18] = (unsigned char)(type >> 12);
        strcpy((char*)record + 19, name);
        used += length;
        f->offset = ordinal;
    }
    return (int64_t)used;
}
static int64_t fcntl_file(GnuVfsState* s, int fd, int command, uint64_t argument) {
    GnuDescriptor* d = descriptor(s, fd);
    if (!d) return -GNU_EBADF;
    switch (command) {
        case 0:
        case 1030:
            if (argument >= GNU_VFS_FD_MAX) return -GNU_EINVAL;
            return install(s, d->file, command == 1030, (int)argument);
        case 1: return d->flags;
        case 2: d->flags = (unsigned)argument & 1; return 0;
        case 3: return d->file->flags;
        case 4:
            d->file->flags = (d->file->flags & ~(GNU_O_APPEND | GNU_O_NONBLOCK)) |
                             ((unsigned)argument & (GNU_O_APPEND | GNU_O_NONBLOCK));
            return 0;
        default: return -GNU_EINVAL;
    }
}
bool gnu_vfs_syscall(GnuVfs* vfs, GnuMemory* memory, uint64_t number, const uint64_t a[6], int64_t* result) {
    GnuVfsState* s = vfs->state;
    char path[GNU_VFS_PATH_MAX];
    GnuNode* node = NULL;
    GnuDescriptor* d;
    int error = 0, fdlink = -1;
    switch (number) {
        case 17:
            error = node_path(s->cwd, path, sizeof path);
            if (error) {
                *result = error;
                return true;
            }
            if (strlen(path) + 1 > a[1]) {
                *result = -GNU_ERANGE;
                return true;
            }
            error = gnu_memory_write(memory, a[0], path, strlen(path) + 1);
            *result = error ? error : (int64_t)strlen(path) + 1;
            return true;
        case 23:
            d = descriptor(s, (int)a[0]);
            *result = d ? install(s, d->file, 0, 0) : -GNU_EBADF;
            return true;
        case 24:
            d = descriptor(s, (int)a[0]);
            if (!d) {
                *result = -GNU_EBADF;
                return true;
            }
            if (a[0] == a[1] || (a[2] & ~GNU_O_CLOEXEC)) {
                *result = -GNU_EINVAL;
                return true;
            }
            if (a[1] >= GNU_VFS_FD_MAX) {
                *result = -GNU_EBADF;
                return true;
            }
            drop_descriptor(&s->descriptors[a[1]]);
            s->descriptors[a[1]].file = d->file;
            s->descriptors[a[1]].flags = a[2] ? 1 : 0;
            ++d->file->references;
            *result = (int64_t)a[1];
            return true;
        case 25: *result = fcntl_file(s, (int)a[0], (int)a[1], a[2]); return true;
        case 29: *result = descriptor(s, (int)a[0]) ? -GNU_ENOTTY : -GNU_EBADF; return true;
        case 34:
        case 35:
        case 48:
        case 49:
        case 56:
        case 78:
        case 79:
        case 439:
            error = gnu_memory_read_string(memory, number == 49 ? a[0] : a[1], path, sizeof path);
            if (error) {
                *result = error;
                return true;
            }
            break;
        case 46:
            d = descriptor(s, (int)a[0]);
            if (!d) {
                *result = -GNU_EBADF;
                return true;
            }
            if ((d->file->flags & GNU_O_PATH) || !(d->file->flags & GNU_O_ACCMODE)) {
                *result = -GNU_EBADF;
                return true;
            }
            if (d->file->node->kind != GNU_NODE_FILE || (int64_t)a[1] < 0) {
                *result = -GNU_EINVAL;
                return true;
            }
            *result = a[1] > GNU_VFS_FILE_MAX ? -GNU_EFBIG : gnu_tmpfs_resize(d->file->node, (size_t)a[1]);
            return true;
        case 50:
            d = descriptor(s, (int)a[0]);
            if (!d)
                *result = -GNU_EBADF;
            else if (d->file->node->kind != GNU_NODE_DIR)
                *result = -GNU_ENOTDIR;
            else {
                s->cwd = d->file->node;
                *result = 0;
            }
            return true;
        case 57:
            d = descriptor(s, (int)a[0]);
            if (!d)
                *result = -GNU_EBADF;
            else {
                drop_descriptor(d);
                *result = 0;
            }
            return true;
        case 61: *result = directory_entries(s, memory, (int)a[0], a[1], a[2]); return true;
        case 62: *result = seek(s, (int)a[0], (int64_t)a[1], (unsigned)a[2]); return true;
        case 63:
        case 64:
        case 67:
        case 68:
            *result =
                io(s, memory, (int)a[0], a[1], a[2], number == 64 || number == 68, number == 67 || number == 68, a[3]);
            return true;
        case 65:
        case 66: *result = vectored_io(s, memory, (int)a[0], a[1], a[2], number == 66); return true;
        case 80:
            d = descriptor(s, (int)a[0]);
            *result = d ? stat_write(s, memory, d->file->node, a[1], d->file->fdlink) : -GNU_EBADF;
            return true;
        default: return false;
    }
    switch (number) {
        case 56: *result = open_file(s, memory, (int)a[0], path, (unsigned)a[2], (unsigned)a[3]); return true;
        case 34: {
            GnuNode* parent;
            char name[256];
            error = parent_path(s, (int)a[0], path, &parent, name);
            if (!error && child(parent, name)) error = -GNU_EEXIST;
            if (!error && parent->deleted) error = -GNU_ENOENT;
            if (!error && parent->readonly) error = -GNU_EROFS;
            if (!error && !(parent->mode & 0200)) error = -GNU_EACCES;
            if (!error && !gnu_vfs_node(s, parent, name, GNU_NODE_DIR, (unsigned)a[2] & 0777u, 0)) error = -GNU_ENOMEM;
            *result = error;
            return true;
        }
        case 35:
            if (a[2] & ~GNU_AT_REMOVEDIR) {
                *result = -GNU_EINVAL;
                return true;
            }
            if (a[2] & GNU_AT_REMOVEDIR) {
                size_t length = strlen(path);
                while (length > 1 && path[length - 1] == '/') --length;
                size_t start = length;
                while (start && path[start - 1] != '/') --start;
                if (length - start == 1 && path[start] == '.') {
                    *result = -GNU_EINVAL;
                    return true;
                }
                if (length - start == 2 && path[start] == '.' && path[start + 1] == '.') {
                    *result = -GNU_ENOTEMPTY;
                    return true;
                }
            }
            error = resolve(s, (int)a[0], path, 0, &node, &fdlink);
            if (!error && (!node->parent || node->parent->readonly || fdlink >= 0)) error = -GNU_EROFS;
            if (!error && (a[2] & GNU_AT_REMOVEDIR) && node->kind != GNU_NODE_DIR) error = -GNU_ENOTDIR;
            if (!error && !(a[2] & GNU_AT_REMOVEDIR) && node->kind == GNU_NODE_DIR) error = -GNU_EISDIR;
            if (!error && node->kind == GNU_NODE_DIR)
                for (GnuNode* c = node->children; c; c = c->sibling)
                    if (!c->deleted) {
                        error = -GNU_ENOTEMPTY;
                        break;
                    }
            if (!error) node->deleted = 1;
            *result = error;
            return true;
        case 48:
        case 439:
            if (a[2] & ~7u || (number == 439 && (a[3] & ~(GNU_AT_SYMLINK_NOFOLLOW | 512u)))) {
                *result = -GNU_EINVAL;
                return true;
            }
            error = resolve(s, (int)a[0], path, number != 439 || !(a[3] & GNU_AT_SYMLINK_NOFOLLOW), &node, NULL);
            if (!error && (a[2] & 2) && node->readonly) error = -GNU_EACCES;
            if (!error && (a[2] & 1) && !(node->mode & 0111)) error = -GNU_EACCES;
            *result = error;
            return true;
        case 49:
            error = resolve(s, GNU_AT_FDCWD, path, 1, &node, NULL);
            if (!error && node->kind != GNU_NODE_DIR) error = -GNU_ENOTDIR;
            if (!error) s->cwd = node;
            *result = error;
            return true;
        case 78: {
            if (!a[3]) {
                *result = -GNU_EINVAL;
                return true;
            }
            char target[GNU_VFS_PATH_MAX];
            error = readlink_path(s, (int)a[0], path, target);
            if (error) {
                *result = error;
                return true;
            }
            size_t size = strlen(target);
            if (size > a[3]) size = (size_t)a[3];
            error = gnu_memory_write(memory, a[2], target, size);
            *result = error ? error : (int64_t)size;
            return true;
        }
        case 79:
            if (a[3] & ~(GNU_AT_SYMLINK_NOFOLLOW | GNU_AT_EMPTY_PATH | 2048u)) {
                *result = -GNU_EINVAL;
                return true;
            }
            if (!*path && (a[3] & GNU_AT_EMPTY_PATH)) {
                if ((int)a[0] == GNU_AT_FDCWD)
                    node = s->cwd;
                else {
                    d = descriptor(s, (int)a[0]);
                    if (!d)
                        error = -GNU_EBADF;
                    else {
                        node = d->file->node;
                        fdlink = d->file->fdlink;
                    }
                }
            } else
                error = resolve(s, (int)a[0], path, !(a[3] & GNU_AT_SYMLINK_NOFOLLOW), &node, &fdlink);
            *result =
                error ? error
                      : stat_write(s, memory, node, a[2], (!*path || (a[3] & GNU_AT_SYMLINK_NOFOLLOW)) ? fdlink : -1);
            return true;
        default: return false;
    }
}
