/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_GNU_MEMORY_H
#define SNX_GNU_MEMORY_H
#include <stdint.h>
#include <stddef.h>
#define GNU_PAGE_SIZE     4096u
#define GNU_PROT_READ     1u
#define GNU_PROT_WRITE    2u
#define GNU_PROT_EXEC     4u
#define GNU_PAGE_MAPPED   8u
#define GNU_ADDRESS_LIMIT UINT64_C(0x100000000)
typedef struct GnuMemory {
    unsigned char* base;
    unsigned char* pages;
    uint64_t size, mmap_next, brk_base, brk_current, brk_limit;
} GnuMemory;
int gnu_memory_init(GnuMemory* memory);
void gnu_memory_destroy(GnuMemory* memory);
void* gnu_memory_access(GnuMemory* memory, uint64_t address, size_t length, unsigned permissions);
int gnu_memory_read(GnuMemory* memory, uint64_t address, void* destination, size_t length);
int gnu_memory_write(GnuMemory* memory, uint64_t address, const void* source, size_t length);
int gnu_memory_read_string(GnuMemory* memory, uint64_t address, char* destination, size_t capacity);
int gnu_memory_map(GnuMemory* memory, uint64_t address, uint64_t length, unsigned permissions, int replace);
int gnu_memory_unmap(GnuMemory* memory, uint64_t address, uint64_t length);
int gnu_memory_protect(GnuMemory* memory, uint64_t address, uint64_t length, unsigned permissions);
int64_t gnu_memory_mmap(GnuMemory* memory, uint64_t address, uint64_t length, unsigned permissions, uint64_t flags);
uint64_t gnu_memory_brk(GnuMemory* memory, uint64_t requested);
size_t gnu_memory_maps(GnuMemory* memory, char* destination, size_t capacity);
#endif
