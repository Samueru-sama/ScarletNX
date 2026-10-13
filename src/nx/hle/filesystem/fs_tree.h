/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_FS_TREE_H
#define SNX_FS_TREE_H

#include "runtime/platform/plat.h"

#define FS_TREE_PATH_MAX 4096

typedef enum {
    FS_TREE_OK = 0,
    FS_TREE_NOTFOUND,
    FS_TREE_EXISTS,
    FS_TREE_NOTEMPTY,
    FS_TREE_INVALID,
    FS_TREE_TOOLONG,
    FS_TREE_IO,
    FS_TREE_NOMEM,
    FS_TREE_READONLY,
    FS_TREE_NOTDELETABLE,
    FS_TREE_NOTRENAMABLE
} FsTreeResult;

typedef struct FsNode {
    char* name;
    struct FsNode *parent, *children, *next;
    u64 size;
    int is_dir, is_virtual, readonly, attached;
    u8* virtual_data;
    unsigned refs; /* open handles; attached nodes are additionally owned by the tree */
} FsNode;

typedef struct {
    char* base;   /* absolute game data directory, captured once at initialization */
    FsNode* root; /* the data directory; its child "root" represents the guest's / */
    PMutex lock;
} FsTree;

/* Initialization is the only operation that enumerates or probes host entries. A failed scan leaves no tree.
   Symlinks, junctions, hardlinked regular files, and non-file/non-directory entries are rejected during scanning.
   All other calls and direct node access require tree->lock to be held. Children are sorted by strcmp(name).
   Close open handles before destroy; retain/release keep removed entries alive while their handles exist. */
FsTreeResult fs_tree_init(FsTree* tree, const char* directory);
void fs_tree_destroy(FsTree* tree);
/* With no scope, accepts drive:/path, /path (under root/), or paths relative to the data directory.
   A scope makes paths relative to that directory; drive prefixes and traversal above scope are rejected. */
FsTreeResult fs_tree_lookup(FsTree* tree, FsNode* scope, const char* path, FsNode** out);
FsTreeResult fs_tree_path(FsTree* tree, const FsNode* node, char* out, size_t capacity);
FsTreeResult fs_tree_mkdirs(FsTree* tree, const char* relative_path, FsNode** out);
FsTreeResult fs_tree_create(FsTree* tree, FsNode* scope, const char* path, int is_dir, u64 size, FsNode** out);
FsTreeResult fs_tree_remove(FsTree* tree, FsNode* scope, const char* path, int is_dir, int recursive, int keep);
FsTreeResult fs_tree_rename(FsTree* tree, FsNode* scope, const char* src, const char* dst, int is_dir);
/* Copies the contents. Missing parent directories are virtual, writable placeholders. Injected files are readonly. */
FsTreeResult fs_tree_add_virtual(FsTree* tree, const char* relative_path, const void* data, u64 size, FsNode** out);
void fs_tree_retain(FsNode* node);
void fs_tree_release(FsNode* node);

#endif
