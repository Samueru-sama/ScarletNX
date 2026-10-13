/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_GNU_ELF_H
#define SNX_GNU_ELF_H
#include "../../memory/memory.h"
typedef struct GnuElfRange {
    uint64_t address;
    const unsigned char* data;
    size_t size;
} GnuElfRange;
typedef struct GnuElf {
    unsigned char* image;
    size_t size;
    uint64_t entry, bias, phdr, image_end;
    uint16_t phnum, phentsize;
    unsigned stack_permissions;
    GnuElfRange* code;
    size_t code_count;
} GnuElf;
int gnu_elf_load(GnuElf* elf, GnuMemory* memory, const char* path, char* error, size_t error_capacity);
int gnu_elf_load_image(GnuElf* elf, GnuMemory* memory, const void* image, size_t size, char* error,
                       size_t error_capacity);
void gnu_elf_destroy(GnuElf* elf);
#endif
