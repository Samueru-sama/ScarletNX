/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "filesystem/internal.h"
#include <stdlib.h>
#include <string.h>

int gnu_tmpfs_resize(GnuNode* node, size_t size) {
    if (size > GNU_VFS_FILE_MAX) return -GNU_EFBIG;
    if (size > node->capacity) {
        size_t capacity = node->capacity ? node->capacity : 256;
        while (capacity < size) capacity *= 2;
        unsigned char* bytes = (unsigned char*)realloc(node->bytes, capacity);
        if (!bytes) return -GNU_ENOSPC;
        node->bytes = bytes;
        node->capacity = capacity;
    }
    if (size > node->size) memset(node->bytes + node->size, 0, size - node->size);
    node->size = size;
    return 0;
}
