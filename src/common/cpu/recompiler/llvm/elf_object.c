/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* ELF64 x86-64 relocatable objects: parsing, symbol resolution and relocation for link_elf.c and code_loader.c */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L /* snprintf */
#include "elf_object.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail(char* error, size_t error_size, const char* format, const char* a, const char* b) {
    snprintf(error, error_size, format, a, b);
    return -1;
}

const char* snx_elf_symbol_name(const SnxElfObject* o, const Elf64_Sym* s) {
    if (ELF64_ST_TYPE(s->st_info) == STT_SECTION) return "<section>";
    return o->strtab + s->st_name;
}

const char* snx_elf_section_name(const SnxElfObject* o, size_t index) {
    return (const char*)o->input->data + o->sh[o->eh->e_shstrndx].sh_offset + o->sh[index].sh_name;
}

int snx_elf_parse(SnxElfObject* o, const SnxLinkInput* input, char* error, size_t error_size) {
    size_t i;
    memset(o, 0, sizeof(*o));
    o->input = input;
    o->eh = (const Elf64_Ehdr*)input->data;
    if (input->size < sizeof(Elf64_Ehdr) || memcmp(o->eh->e_ident, ELFMAG, SELFMAG) ||
        o->eh->e_ident[EI_CLASS] != ELFCLASS64 || o->eh->e_ident[EI_DATA] != ELFDATA2LSB || o->eh->e_type != ET_REL ||
        o->eh->e_machine != EM_X86_64 || o->eh->e_shentsize != sizeof(Elf64_Shdr) ||
        o->eh->e_shoff + (uint64_t)o->eh->e_shnum * sizeof(Elf64_Shdr) > input->size)
        return fail(error, error_size, "%s: not an x86-64 ELF relocatable object%s", input->name, "");
    o->sh = (const Elf64_Shdr*)(input->data + o->eh->e_shoff);
    /* one block, the 8-byte arrays first */
    o->address = (uint64_t*)calloc(o->eh->e_shnum, 2 * sizeof(uint64_t) + sizeof(int));
    if (!o->address) return fail(error, error_size, "out of memory%s%s", "", "");
    o->position = o->address + o->eh->e_shnum;
    o->kind = (int*)(o->position + o->eh->e_shnum);
    for (i = 0; i < o->eh->e_shnum; ++i) {
        const Elf64_Shdr* s = &o->sh[i];
        o->kind[i] = -1;
        if (s->sh_type != SHT_NOBITS && s->sh_offset + s->sh_size > input->size)
            return fail(error, error_size, "%s: truncated section%s", input->name, "");
        if (s->sh_type == SHT_SYMTAB) {
            o->syms = (const Elf64_Sym*)(input->data + s->sh_offset);
            o->nsyms = s->sh_size / sizeof(Elf64_Sym);
            if (s->sh_link >= o->eh->e_shnum) return fail(error, error_size, "%s: bad symbol table%s", input->name, "");
            o->strtab = (const char*)input->data + o->sh[s->sh_link].sh_offset;
        }
        if (!(s->sh_flags & SHF_ALLOC)) continue;
        if (s->sh_flags & SHF_TLS)
            return fail(error, error_size, "%s: thread-local data is not supported%s", input->name, "");
        if (s->sh_type == SHT_INIT_ARRAY || s->sh_type == SHT_FINI_ARRAY || s->sh_type == SHT_PREINIT_ARRAY)
            return fail(error, error_size, "%s: constructors are not supported%s", input->name, "");
        if (s->sh_type == SHT_NOBITS)
            o->kind[i] = SNX_ELF_BSS;
        else if (s->sh_type == SHT_PROGBITS || s->sh_type == SHT_X86_64_UNWIND)
            o->kind[i] = s->sh_flags & SHF_EXECINSTR ? SNX_ELF_TEXT
                         : s->sh_flags & SHF_WRITE   ? SNX_ELF_DATA
                                                     : SNX_ELF_RODATA;
        else if (s->sh_type == SHT_NOTE || s->sh_type == SHT_GROUP)
            continue;
        else
            return fail(error, error_size, "%s: unsupported allocated section type%s", input->name, "");
    }
    if (!o->syms) return fail(error, error_size, "%s: no symbol table%s", input->name, "");
    return 0;
}

void snx_elf_free(SnxElfObject* o) {
    free(o->address);
    o->address = o->position = NULL;
    o->kind = NULL;
}

int snx_elf_collect_globals(const SnxElfObject* objs, size_t count, const SnxLinkNames* loaded, SnxLinkNames* globals,
                            char* error, size_t error_size) {
    size_t i, j, found;
    for (i = 0; i < count; ++i) {
        const SnxElfObject* o = &objs[i];
        for (j = 1; j < o->nsyms; ++j) {
            const Elf64_Sym* s = &o->syms[j];
            int bind = ELF64_ST_BIND(s->st_info);
            const char* name;
            if ((bind != STB_GLOBAL && bind != STB_WEAK) || s->st_shndx == SHN_UNDEF) continue;
            name = snx_elf_symbol_name(o, s);
            if (s->st_shndx == SHN_COMMON) return fail(error, error_size, "%s: common symbol %s", o->input->name, name);
            if (loaded && snx_link_find(loaded, name, &found)) {
                if (bind == STB_WEAK) continue;
                return fail(error, error_size, "duplicate symbol %s (%s defines one already loaded)", name,
                            o->input->name);
            }
            if (snx_link_find(globals, name, &found)) {
                const SnxElfObject* other = &objs[found >> 32];
                if (bind == STB_WEAK) continue;
                if (ELF64_ST_BIND(other->syms[found & 0xFFFFFFFFu].st_info) != STB_WEAK)
                    return fail(error, error_size, "duplicate symbol %s (%s)", name, o->input->name);
            }
            snx_link_insert(globals, name, (i << 32) | j);
        }
    }
    return 0;
}

int snx_elf_definition(const SnxElfObject* objs, const SnxLinkNames* globals, size_t object, size_t index,
                       size_t* def_object, size_t* def_index, char* error, size_t error_size) {
    const SnxElfObject* o = &objs[object];
    const Elf64_Sym* s;
    size_t found;
    if (index >= o->nsyms) return fail(error, error_size, "%s: bad symbol index%s", o->input->name, "");
    s = &o->syms[index];
    if (s->st_shndx != SHN_UNDEF) {
        *def_object = object;
        *def_index = index;
        return SNX_ELF_HERE;
    }
    if (snx_link_find(globals, snx_elf_symbol_name(o, s), &found)) {
        *def_object = found >> 32;
        *def_index = found & 0xFFFFFFFFu;
        return SNX_ELF_HERE;
    }
    return SNX_ELF_ELSEWHERE;
}

uint64_t snx_elf_symbol_address(const SnxElfObject* objs, size_t object, size_t index, int* absolute) {
    const SnxElfObject* o = &objs[object];
    const Elf64_Sym* s = &o->syms[index];
    *absolute = s->st_shndx == SHN_ABS;
    if (*absolute) return s->st_value;
    return o->address[s->st_shndx] + s->st_value;
}

int snx_elf_got_relocation(unsigned type) {
    return type == R_X86_64_GOTPCREL || type == R_X86_64_GOTPCRELX || type == R_X86_64_REX_GOTPCRELX;
}

void snx_elf_layout(SnxElfObject* objs, size_t count, uint64_t* kind_size, uint64_t* kind_align) {
    size_t i, j, k;
    for (k = 0; k < SNX_ELF_KINDS; ++k) {
        kind_size[k] = 0;
        kind_align[k] = 1;
    }
    for (k = 0; k < SNX_ELF_KINDS; ++k)
        for (i = 0; i < count; ++i) {
            SnxElfObject* o = &objs[i];
            for (j = 0; j < o->eh->e_shnum; ++j) {
                uint64_t align = o->sh[j].sh_addralign ? o->sh[j].sh_addralign : 1;
                if (o->kind[j] != (int)k) continue;
                kind_size[k] = (kind_size[k] + align - 1) / align * align;
                o->position[j] = kind_size[k];
                kind_size[k] += o->sh[j].sh_size;
                if (align > kind_align[k]) kind_align[k] = align;
            }
        }
}

/* the bytes a relocation writes (4 for the types snx_elf_relocate refuses) */
static size_t field_size(unsigned type) {
    switch (type) {
        case R_X86_64_NONE: return 0;
        case R_X86_64_64:
        case R_X86_64_PC64:
        case R_X86_64_GOTOFF64:
        case R_X86_64_GOTPC64: return 8;
        default: return 4;
    }
}

int snx_elf_relocate(unsigned char* p, size_t room, unsigned type, uint64_t S, int64_t A, uint64_t P, uint64_t got,
                     uint64_t slot) {
    if (field_size(type) > room) return SNX_LINK_OUTSIDE;
    switch (type) {
        case R_X86_64_NONE: return 0;
        case R_X86_64_64: snx_link_put64(p, S + (uint64_t)A); return 0;
        case R_X86_64_PC32:
        case R_X86_64_PLT32: return snx_link_rel32(p, S + (uint64_t)A, P);
        case R_X86_64_PC64: snx_link_put64(p, S + (uint64_t)A - P); return 0;
        case R_X86_64_GOTPCREL:
        case R_X86_64_GOTPCRELX:
        case R_X86_64_REX_GOTPCRELX: return snx_link_rel32(p, slot + (uint64_t)A, P);
        case R_X86_64_GOTOFF64: snx_link_put64(p, S + (uint64_t)A - got); return 0;
        case R_X86_64_GOTPC32: return snx_link_rel32(p, got + (uint64_t)A, P);
        case R_X86_64_GOTPC64: snx_link_put64(p, got + (uint64_t)A - P); return 0;
        default: return SNX_LINK_UNSUPPORTED;
    }
}
#else
typedef int snx_elf_object_unused; /* ISO C wants a declaration in every file */
#endif
