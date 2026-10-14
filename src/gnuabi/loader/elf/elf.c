/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Linux ELF64/AArch64 layout, independently decoded rather than host Elf headers. */
#include "elf.h"
#include "../../abi/errno/errno.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
static uint16_t rd16(const unsigned char* p) { return (uint16_t)(p[0] | (unsigned)p[1] << 8); }
static uint32_t rd32(const unsigned char* p) { return (uint32_t)rd16(p) | (uint32_t)rd16(p + 2) << 16; }
static uint64_t rd64(const unsigned char* p) { return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32; }
static uint64_t down(uint64_t n) { return n & ~UINT64_C(4095); }
static uint64_t up(uint64_t n) { return (n + 4095) & ~UINT64_C(4095); }
/* One program header, normalized across ELF32 and ELF64 layouts. */
typedef struct {
    uint32_t pt, flags;
    uint64_t offset, va, filesz, memsz, align;
} Phdr;
static void rd_phdr(const unsigned char* p, unsigned bits, Phdr* h) {
    if (bits == 32) {
        h->pt = rd32(p);
        h->offset = rd32(p + 4);
        h->va = rd32(p + 8);
        h->filesz = rd32(p + 16);
        h->memsz = rd32(p + 20);
        h->flags = rd32(p + 24);
        h->align = rd32(p + 28);
    } else {
        h->pt = rd32(p);
        h->flags = rd32(p + 4);
        h->offset = rd64(p + 8);
        h->va = rd64(p + 16);
        h->filesz = rd64(p + 32);
        h->memsz = rd64(p + 40);
        h->align = rd64(p + 48);
    }
}
static int fail(char* error, size_t cap, const char* format, ...) {
    va_list args;
    va_start(args, format);
    if (cap) vsnprintf(error, cap, format, args);
    va_end(args);
    return -GNU_EINVAL;
}
void gnu_elf_destroy(GnuElf* elf) {
    free(elf->image);
    free(elf->code);
    memset(elf, 0, sizeof *elf);
}
int gnu_elf_load_image(GnuElf* elf, GnuMemory* m, const void* source, size_t size, char* error, size_t cap) {
    const unsigned char* b = source;
    memset(elf, 0, sizeof *elf);
    if (size < 52 || memcmp(b, "\177ELF", 4) || b[5] != 1 || b[6] != 1)
        return fail(error, cap, "Expected a little-endian ELF executable");
    unsigned bits = b[4] == 1 ? 32 : b[4] == 2 ? 64 : 0;
    if (!bits) return fail(error, cap, "Unsupported ELF class");
    elf->bits = bits;
    uint16_t type = rd16(b + 16), machine = rd16(b + 18);
    uint16_t phsize = rd16(b + (bits == 32 ? 42 : 54)), phnum = rd16(b + (bits == 32 ? 44 : 56));
    uint64_t phoff = bits == 32 ? rd32(b + 28) : rd64(b + 32);
    uint64_t entry = bits == 32 ? rd32(b + 24) : rd64(b + 24);
    unsigned want_machine = bits == 32 ? 40 : 183; /* EM_ARM : EM_AARCH64 */
    unsigned want_phsize = bits == 32 ? 32 : 56, want_ehsize = bits == 32 ? 52 : 64;
    if ((type != 2 && type != 3) || machine != want_machine || rd32(b + 20) != 1 ||
        rd16(b + (bits == 32 ? 40 : 52)) != want_ehsize || phsize != want_phsize || !phnum || phnum > 1024 ||
        phoff > size || (uint64_t)phnum * phsize > size - phoff)
        return fail(error, cap, "Invalid or unsupported ELF/program headers");
    elf->thumb = bits == 32 && (entry & 1);
    elf->bias = type == 3 ? UINT64_C(0x10000000) : 0;
    elf->stack_permissions = GNU_PROT_READ | GNU_PROT_WRITE;
    size_t executable_segments = 0, load_segments = 0;
    uint64_t phdr = 0, end = 0;
    Phdr h;
    for (unsigned i = 0; i < phnum; i++) {
        rd_phdr(b + phoff + i * phsize, bits, &h);
        if (h.pt == 3) {
            if (h.offset <= size && h.filesz && h.filesz <= size - h.offset && b[h.offset + h.filesz - 1] == 0)
                return fail(error, cap, "Dynamic ELF interpreter '%s' is unsupported; use a static executable",
                            b + h.offset);
            return fail(error, cap, "Dynamic ELF interpreters are unsupported; use a static executable");
        }
        if (h.pt == 0x6474e551)
            elf->stack_permissions = (h.flags & 4 ? 1u : 0u) | (h.flags & 2 ? 2u : 0u) | (h.flags & 1 ? 4u : 0u);
        if (h.pt != 1) continue;
        if (!h.memsz) {
            if (h.filesz) return fail(error, cap, "PT_LOAD %u has file bytes without a memory interval", i);
            continue;
        }
        if (h.filesz > h.memsz || h.offset > size || h.filesz > size - h.offset || h.va > m->size ||
            elf->bias > m->size - h.va || h.memsz > m->size - h.va - elf->bias ||
            (h.align > 1 && ((h.align & (h.align - 1)) || (h.va & (h.align - 1)) != (h.offset & (h.align - 1)))))
            return fail(error, cap, "PT_LOAD %u is invalid or outside the bounded guest address space", i);
        uint64_t va = h.va + elf->bias;
        if (va < GNU_PAGE_SIZE || up(va + h.memsz) > UINT64_C(0x40000000))
            return fail(error, cap, "PT_LOAD %u overlaps reserved guest regions", i);
        if ((va & 4095) != (h.offset & 4095)) return fail(error, cap, "PT_LOAD %u has inconsistent page offsets", i);
        if (va + h.memsz > end) end = va + h.memsz;
        if (phoff >= h.offset && phoff - h.offset <= h.filesz &&
            (uint64_t)phnum * phsize <= h.filesz - (phoff - h.offset))
            phdr = va + phoff - h.offset;
        if (h.flags & 1) executable_segments++;
        load_segments++;
    }
    if (!load_segments || !executable_segments || entry > m->size - elf->bias)
        return fail(error, cap, "ELF has no executable load segment or has an invalid entry address");
    for (unsigned i = 0; i < phnum; i++) {
        rd_phdr(b + phoff + i * phsize, bits, &h);
        if (h.pt != 1 || !h.memsz) continue;
        uint64_t begin = h.va + elf->bias, finish = begin + h.memsz;
        for (unsigned j = 0; j < i; j++) {
            Phdr g;
            rd_phdr(b + phoff + j * phsize, bits, &g);
            if (g.pt == 1 && g.memsz) {
                uint64_t lo = g.va + elf->bias;
                if (begin < lo + g.memsz && lo < finish)
                    return fail(error, cap, "Overlapping PT_LOAD memory intervals are unsupported");
            }
        }
    }
    elf->image = malloc(size);
    elf->code = calloc(executable_segments, sizeof *elf->code);
    if (!elf->image || !elf->code) {
        gnu_elf_destroy(elf);
        return -GNU_ENOMEM;
    }
    memcpy(elf->image, b, size);
    elf->size = size;
    for (unsigned i = 0; i < phnum; i++) {
        rd_phdr(b + phoff + i * phsize, bits, &h);
        if (h.pt != 1 || !h.memsz) continue;
        uint64_t va = h.va + elf->bias;
        for (uint64_t page = down(va); page < up(va + h.memsz); page += GNU_PAGE_SIZE) {
            if (!(m->pages[page / GNU_PAGE_SIZE] & GNU_PAGE_MAPPED) && gnu_memory_map(m, page, GNU_PAGE_SIZE, 3, 0)) {
                gnu_elf_destroy(elf);
                return fail(error, cap, "Cannot map ELF segment");
            }
        }
        memcpy(m->base + va, b + h.offset, (size_t)h.filesz);
        memset(m->base + va + h.filesz, 0, (size_t)(h.memsz - h.filesz));
        if (h.flags & 1) elf->code[elf->code_count++] = (GnuElfRange){va, m->base + va, (size_t)h.memsz};
    }
    unsigned char* permissions = calloc((size_t)(up(end) / GNU_PAGE_SIZE), 1);
    if (!permissions) {
        gnu_elf_destroy(elf);
        return -GNU_ENOMEM;
    }
    for (unsigned i = 0; i < phnum; i++) {
        rd_phdr(b + phoff + i * phsize, bits, &h);
        if (h.pt != 1 || !h.memsz) continue;
        unsigned prot = (h.flags & 4 ? 1u : 0u) | (h.flags & 2 ? 2u : 0u) | (h.flags & 1 ? 4u : 0u);
        uint64_t va = h.va + elf->bias;
        for (uint64_t page = down(va); page < up(va + h.memsz); page += GNU_PAGE_SIZE)
            permissions[page / GNU_PAGE_SIZE] |= (unsigned char)(GNU_PAGE_MAPPED | prot);
    }
    for (uint64_t i = 0; i < up(end) / GNU_PAGE_SIZE;) {
        if (!permissions[i]) {
            i++;
            continue;
        }
        uint64_t begin = i;
        unsigned p = permissions[i];
        do { i++; } while (i < up(end) / GNU_PAGE_SIZE && permissions[i] == p);
        if (gnu_memory_protect(m, begin * GNU_PAGE_SIZE, (i - begin) * GNU_PAGE_SIZE, p & 7u)) {
            free(permissions);
            gnu_elf_destroy(elf);
            return fail(error, cap, "Cannot protect ELF segments");
        }
    }
    free(permissions);
    elf->entry = (entry & (bits == 32 ? ~1u : ~UINT64_C(0))) + elf->bias;
    unsigned entry_mask = bits == 64 ? 3u : (elf->thumb ? 1u : 3u);
    if (!gnu_memory_access(m, elf->entry, 4, GNU_PROT_EXEC) || (elf->entry & entry_mask)) {
        gnu_elf_destroy(elf);
        return fail(error, cap, "ELF entry is not an aligned executable address");
    }
    if (!phdr) {
        phdr = GNU_PAGE_SIZE;
        uint64_t bytes = (uint64_t)phnum * phsize;
        if (gnu_memory_map(m, phdr, bytes, 3, 0) || gnu_memory_write(m, phdr, b + phoff, (size_t)bytes) ||
            gnu_memory_protect(m, phdr, up(bytes), 1)) {
            gnu_elf_destroy(elf);
            return fail(error, cap, "Cannot create guest ELF program headers");
        }
    }
    elf->phdr = phdr;
    elf->phnum = phnum;
    elf->phentsize = phsize;
    elf->image_end = up(end);
    m->brk_base = m->brk_current = up(end);
    m->brk_limit = up(end) + UINT64_C(0x04000000);
    if (m->brk_limit > UINT64_C(0x40000000)) m->brk_limit = UINT64_C(0x40000000);
    if (cap) error[0] = 0;
    return 0;
}
int gnu_elf_load(GnuElf* elf, GnuMemory* memory, const char* path, char* error, size_t cap) {
    FILE* file = fopen(path, "rb");
    if (!file) return fail(error, cap, "Cannot open ELF '%s'", path);
    if (fseek(file, 0, SEEK_END) || ftell(file) < 0) {
        fclose(file);
        return fail(error, cap, "Cannot size ELF");
    }
    long size = ftell(file);
    if (size < 64 || (uint64_t)size > UINT64_C(0x10000000)) {
        fclose(file);
        return fail(error, cap, "ELF size is unsupported");
    }
    unsigned char* bytes = malloc((size_t)size);
    if (!bytes) {
        fclose(file);
        return -GNU_ENOMEM;
    }
    rewind(file);
    size_t n = fread(bytes, 1, (size_t)size, file);
    fclose(file);
    if (n != (size_t)size) {
        free(bytes);
        return fail(error, cap, "Cannot read ELF");
    }
    int result = gnu_elf_load_image(elf, memory, bytes, (size_t)size, error, cap);
    free(bytes);
    return result;
}
