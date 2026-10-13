/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "filesystem/internal.h"
#include "process/process.h"
#include <string.h>

int gnu_devfs_init(GnuVfsState* state) {
    static const char* names[] = {"null", "zero", "urandom", "random"};
    GnuNode* dev = gnu_vfs_node(state, state->root, "dev", GNU_NODE_DIR, 0755, 1);
    if (!dev) return -GNU_ENOMEM;
    for (unsigned i = 0; i < 4; ++i) {
        GnuNode* node = gnu_vfs_node(state, dev, names[i], GNU_NODE_DEVICE, 0666, 0);
        if (!node) return -GNU_ENOMEM;
        node->subtype = i < 2 ? i : GNU_DEV_RANDOM;
    }
    return gnu_vfs_node(state, dev, "fd", GNU_NODE_DIR, 0555, 1) ? 0 : -GNU_ENOMEM;
}
int64_t gnu_devfs_read(unsigned kind, void* bytes, size_t length) {
    if (kind == GNU_DEV_NULL) return 0;
    if (kind == GNU_DEV_ZERO)
        memset(bytes, 0, length);
    else {
        int error = gnu_random_bytes(bytes, length);
        if (error) return error;
    }
    return (int64_t)length;
}
