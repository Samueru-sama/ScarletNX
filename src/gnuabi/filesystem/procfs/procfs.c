/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "filesystem/internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int gnu_procfs_init(GnuVfsState* state) {
    static const char* names[] = {"maps", "cmdline", "status"};
    GnuNode* proc = gnu_vfs_node(state, state->root, "proc", GNU_NODE_DIR, 0555, 1);
    if (!proc) return -GNU_ENOMEM;
    GnuNode* self = gnu_vfs_node(state, proc, "self", GNU_NODE_DIR, 0555, 1);
    if (!self) return -GNU_ENOMEM;
    for (unsigned i = 0; i < 3; ++i) {
        GnuNode* node = gnu_vfs_node(state, self, names[i], GNU_NODE_PROC, 0444, 1);
        if (!node) return -GNU_ENOMEM;
        node->subtype = i;
    }
    if (!gnu_vfs_node(state, self, "exe", GNU_NODE_LINK, 0777, 1) ||
        !gnu_vfs_node(state, self, "fd", GNU_NODE_DIR, 0555, 1))
        return -GNU_ENOMEM;
    GnuNode* cpu = gnu_vfs_node(state, proc, "cpuinfo", GNU_NODE_PROC, 0444, 1);
    if (!cpu) return -GNU_ENOMEM;
    cpu->subtype = GNU_PROC_CPUINFO;
    return 0;
}
int gnu_procfs_snapshot(GnuVfsState* state, GnuMemory* memory, unsigned kind, unsigned char** bytes, size_t* length) {
    char text[1024];
    const void* source = text;
    size_t size = 0;
    if (kind == GNU_PROC_MAPS) {
        /* Keep a stable open-file snapshot, as seq_file does for a read sequence. */
        size_t capacity = 1024 * 1024;
        unsigned char* maps = (unsigned char*)malloc(capacity);
        if (!maps) return -GNU_ENOMEM;
        size = gnu_memory_maps(memory, (char*)maps, capacity);
        if (size >= capacity) {
            free(maps);
            return -GNU_EOVERFLOW;
        }
        *bytes = maps;
        *length = size;
        return 0;
    }
    if (kind == GNU_PROC_CMDLINE) {
        source = state->cmdline;
        size = state->cmdline_size;
    } else if (kind == GNU_PROC_STATUS) {
        int count = snprintf(text, sizeof text,
                             "Name:\tscarletnx-gnuabi\nState:\tR (running)\nTgid:\t%u\nPid:\t%u\nPPid:\t1\n"
                             "Uid:\t1000\t1000\t1000\t1000\nGid:\t1000\t1000\t1000\t1000\nThreads:\t1\n",
                             state->pid, state->pid);
        if (count < 0 || (size_t)count >= sizeof text) return -GNU_EIO;
        size = (size_t)count;
    } else if (kind == GNU_PROC_CPUINFO) {
        /* Report guest ISA information; host x86 CPU features are irrelevant here. */
        source = "processor\t: 0\nBogoMIPS\t: 100.00\nFeatures\t: fp asimd\nCPU implementer\t: 0x41\n"
                 "CPU architecture: 8\nCPU variant\t: 0x0\nCPU part\t: 0xd07\nCPU revision\t: 0\n";
        size = strlen((const char*)source);
    }
    unsigned char* data = (unsigned char*)malloc(size ? size : 1);
    if (!data) return -GNU_ENOMEM;
    if (size) memcpy(data, source, size);
    *bytes = data;
    *length = size;
    return 0;
}
