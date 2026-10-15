/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The JIT's loader (code_loader.h): a code arena, and the LLVM backend's ELF objects linked into it */
#define _POSIX_C_SOURCE 200809L /* snprintf */
#include "code_loader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the table keeps addresses in SnxLinkNames' size_t values */
typedef char snx_code_loader_needs_64_bits[sizeof(size_t) >= 8 ? 1 : -1];

struct SnxCodeLoader {
    unsigned char* base; /* the arena */
    size_t reserved, used, page;
    SnxLinkNames symbols; /* name -> address: the globals of earlier loads and the defined names */
};

SnxCodeLoader* snx_code_loader_new(size_t reserve, char* error, size_t error_size) {
    SnxCodeLoader* loader;
    size_t page = snx_vm_page_size();
    if (!reserve) reserve = SNX_CODE_ARENA_DEFAULT;
    if (reserve > (size_t)1 << 31) {
        snprintf(error, error_size, "a code arena holds at most 2 GiB, so 32-bit displacements reach across it");
        return NULL;
    }
    reserve = (reserve + page - 1) / page * page;
    loader = (SnxCodeLoader*)calloc(1, sizeof(*loader));
    if (!loader) {
        snprintf(error, error_size, "out of memory");
        return NULL;
    }
    loader->base = (unsigned char*)snx_vm_reserve(reserve);
    if (!loader->base) {
        snprintf(error, error_size, "cannot reserve %lu MiB of address space for code", (unsigned long)(reserve >> 20));
        free(loader);
        return NULL;
    }
    loader->reserved = reserve;
    loader->page = page;
    return loader;
}

void snx_code_loader_free(SnxCodeLoader* loader) {
    if (!loader) return;
    snx_vm_release(loader->base, loader->reserved);
    snx_link_names_free(&loader->symbols);
    free(loader);
}

int snx_code_define(SnxCodeLoader* loader, const char* name, SnxU64 address) {
    size_t found;
    if (snx_link_find(&loader->symbols, name, &found)) return -1;
    snx_link_insert(&loader->symbols, name, (size_t)address);
    return 0;
}

int snx_code_lookup(const SnxCodeLoader* loader, const char* name, SnxU64* address) {
    size_t found;
    if (!snx_link_find(&loader->symbols, name, &found)) return 0;
    *address = found;
    return 1;
}

size_t snx_code_used(const SnxCodeLoader* loader) { return loader->used; }

#ifndef _WIN32
#include "elf_object.h"

/* jmp *slot(%rip), and two int3 to the next stub: the call of a table symbol beyond 32-bit reach, through its GOT
   slot */
#define STUB_SIZE 8

/* One snx_code_load call. Addresses are absolute, the arena's [arena_start, arena_end) */
typedef struct {
    SnxElfObject* objs;
    size_t count;
    SnxLinkNames globals; /* the unit's own: name -> object << 32 | symbol index */
    SnxLinkNames got;     /* "object:symbol" of a unit definition, or "@address" of a table symbol -> GOT slot */
    SnxLinkNames stubs;   /* "@address" of a far table symbol that code calls -> stub */
    size_t got_count, stub_count;
    uint64_t arena_start, arena_end, got_address, stub_address;
} Load;

/* What a relocation's symbol resolves to */
typedef struct {
    uint64_t address;
    int far;      /* a table symbol outside the arena, which a 32-bit displacement may not reach */
    char key[48]; /* its GOT slot's and stub's key; empty for the GOT itself and an undefined weak symbol (address 0) */
} Target;

static uint64_t align_up(uint64_t value, uint64_t alignment) { return (value + alignment - 1) / alignment * alignment; }

static unsigned char* at(const SnxCodeLoader* loader, const Load* ld, uint64_t address) {
    return loader->base + (size_t)(address - ld->arena_start);
}

/* The unit's definitions first, then the table, as code_loader.h says. The unit's addresses are final once the
   sections are placed; before, only the key is */
static int resolve(const SnxCodeLoader* loader, const Load* ld, size_t object, size_t index, Target* t, char* error,
                   size_t error_size) {
    const SnxElfObject* o = &ld->objs[object];
    size_t def_object, def_index, found;
    int absolute, where;
    const char* name;
    memset(t, 0, sizeof(*t));
    where = snx_elf_definition(ld->objs, &ld->globals, object, index, &def_object, &def_index, error, error_size);
    if (where < 0) return -1;
    if (where == SNX_ELF_HERE) {
        t->address = snx_elf_symbol_address(ld->objs, def_object, def_index, &absolute);
        sprintf(t->key, "%lu:%lu", (unsigned long)def_object, (unsigned long)def_index);
        return 0;
    }
    name = snx_elf_symbol_name(o, &o->syms[index]);
    if (snx_link_find(&loader->symbols, name, &found)) {
        t->address = found;
        t->far = t->address < ld->arena_start || t->address >= ld->arena_end;
        sprintf(t->key, "@%lx", (unsigned long)found);
        return 0;
    }
    if (!strcmp(name, "_GLOBAL_OFFSET_TABLE_")) {
        t->address = ld->got_address;
        return 0;
    }
    if (ELF64_ST_BIND(o->syms[index].st_info) == STB_WEAK) return 0;
    snprintf(error, error_size, "undefined symbol %s (referenced by %s)", name, o->input->name);
    return -1;
}

/* Section number section when it holds the relocations of a loaded section, which the two passes walk; else NULL */
static const Elf64_Shdr* relocations(const SnxElfObject* o, size_t section) {
    const Elf64_Shdr* s = &o->sh[section];
    if (s->sh_type != SHT_RELA || s->sh_info >= o->eh->e_shnum || o->kind[s->sh_info] < 0) return NULL;
    return s;
}

/* First pass: the GOT slots and stubs the unit needs */
static int scan(const SnxCodeLoader* loader, Load* ld, char* error, size_t error_size) {
    size_t i, j, k, n, slot;
    Target t;
    for (i = 0; i < ld->count; ++i) {
        const SnxElfObject* o = &ld->objs[i];
        for (j = 0; j < o->eh->e_shnum; ++j) {
            const Elf64_Shdr* s = relocations(o, j);
            const Elf64_Rela* r;
            if (o->sh[j].sh_type == SHT_REL) {
                snprintf(error, error_size, "%s: REL relocations are not supported", o->input->name);
                return -1;
            }
            if (!s) continue;
            r = (const Elf64_Rela*)(o->input->data + s->sh_offset);
            n = s->sh_size / sizeof(Elf64_Rela);
            for (k = 0; k < n; ++k) {
                unsigned type = ELF64_R_TYPE(r[k].r_info);
                if (resolve(loader, ld, i, ELF64_R_SYM(r[k].r_info), &t, error, error_size)) return -1;
                if (snx_elf_got_relocation(type)) {
                    if (!t.key[0]) {
                        snprintf(error, error_size, "%s: GOT reference to an undefined weak symbol", o->input->name);
                        return -1;
                    }
                    if (!snx_link_find(&ld->got, t.key, &slot)) snx_link_insert(&ld->got, t.key, ld->got_count++);
                } else if (type == R_X86_64_PLT32 && t.far && !snx_link_find(&ld->stubs, t.key, &slot)) {
                    snx_link_insert(&ld->stubs, t.key, ld->stub_count++);
                    if (!snx_link_find(&ld->got, t.key, &slot)) snx_link_insert(&ld->got, t.key, ld->got_count++);
                }
            }
        }
    }
    return 0;
}

/* t's GOT slot, written with its address: the slot's own address */
static uint64_t got_slot(const SnxCodeLoader* loader, const Load* ld, const Target* t) {
    size_t slot = 0;
    uint64_t address;
    snx_link_find(&ld->got, t->key, &slot);
    address = ld->got_address + 8 * (uint64_t)slot;
    snx_link_put64(at(loader, ld, address), t->address);
    return address;
}

/* t's stub, written to jump through its GOT slot: the stub's address */
static uint64_t stub(const SnxCodeLoader* loader, const Load* ld, const Target* t) {
    size_t index = 0;
    uint64_t address, slot = got_slot(loader, ld, t);
    unsigned char* p;
    snx_link_find(&ld->stubs, t->key, &index);
    address = ld->stub_address + STUB_SIZE * (uint64_t)index;
    p = at(loader, ld, address);
    p[0] = 0xFF; /* jmp qword ptr [rip + slot - next] */
    p[1] = 0x25;
    snx_link_rel32(p + 2, slot, address + 6); /* both in the arena: in reach */
    return address;
}

/* Second pass: every relocation of the unit, written into its placed sections */
static int relocate(const SnxCodeLoader* loader, const Load* ld, char* error, size_t error_size) {
    size_t i, j, k, n;
    for (i = 0; i < ld->count; ++i) {
        const SnxElfObject* o = &ld->objs[i];
        for (j = 0; j < o->eh->e_shnum; ++j) {
            const Elf64_Shdr* s = relocations(o, j);
            const Elf64_Shdr* target;
            const Elf64_Rela* r;
            if (!s) continue;
            target = &o->sh[s->sh_info];
            r = (const Elf64_Rela*)(o->input->data + s->sh_offset);
            n = s->sh_size / sizeof(Elf64_Rela);
            for (k = 0; k < n; ++k) {
                unsigned type = ELF64_R_TYPE(r[k].r_info);
                uint64_t P = o->address[s->sh_info] + r[k].r_offset, slot = 0;
                unsigned char* p;
                Target t;
                int written = SNX_LINK_OUTSIDE;
                if (resolve(loader, ld, i, ELF64_R_SYM(r[k].r_info), &t, error, error_size)) return -1;
                if (r[k].r_offset <= target->sh_size) {
                    p = at(loader, ld, P);
                    if (snx_elf_got_relocation(type)) slot = got_slot(loader, ld, &t);
                    written = snx_elf_relocate(p, target->sh_size - r[k].r_offset, type, t.address, r[k].r_addend, P,
                                               ld->got_address, slot);
                    if (written == SNX_LINK_RANGE && type == R_X86_64_PLT32 && t.far)
                        written = snx_elf_relocate(p, target->sh_size - r[k].r_offset, type, stub(loader, ld, &t),
                                                   r[k].r_addend, P, ld->got_address, slot);
                }
                if (written == SNX_LINK_OUTSIDE) {
                    snprintf(error, error_size, "%s: relocation outside its section", o->input->name);
                    return -1;
                }
                if (written == SNX_LINK_RANGE) {
                    snprintf(error, error_size, "%s: relocation out of range of %s", o->input->name,
                             t.far ? "a symbol outside the code arena" : "its target");
                    return -1;
                }
                if (written) {
                    snprintf(error, error_size, "%s: unsupported relocation type %u", o->input->name, type);
                    return -1;
                }
            }
        }
    }
    return 0;
}

int snx_code_load(SnxCodeLoader* loader, const SnxLinkInput* inputs, size_t count, const char* const* names,
                  size_t name_count, SnxU64* addresses, char* error, size_t error_size) {
    Load ld;
    uint64_t kind_size[SNX_ELF_KINDS], kind_align[SNX_ELF_KINDS], kind_address[SNX_ELF_KINDS];
    uint64_t start, text_end, read_only, writable, end;
    size_t i, j, found;
    int rc = -1, absolute;
    memset(&ld, 0, sizeof(ld));
    ld.count = count;
    ld.arena_start = (uint64_t)(size_t)loader->base;
    ld.arena_end = ld.arena_start + loader->reserved;
    ld.objs = (SnxElfObject*)calloc(count ? count : 1, sizeof(SnxElfObject));
    if (!ld.objs) {
        snprintf(error, error_size, "out of memory");
        return -1;
    }
    for (i = 0; i < count; ++i)
        if (snx_elf_parse(&ld.objs[i], &inputs[i], error, error_size)) goto done;
    if (snx_elf_collect_globals(ld.objs, count, &loader->symbols, &ld.globals, error, error_size)) goto done;
    for (i = 0; i < name_count; ++i)
        if (!snx_link_find(&ld.globals, names[i], &found) && !snx_link_find(&loader->symbols, names[i], &found)) {
            snprintf(error, error_size, "symbol %s is not defined", names[i]);
            goto done;
        }
    if (scan(loader, &ld, error, error_size)) goto done;
    /* the unit's pages, from the first free one: [code, stubs] [read-only data, GOT] [data, bss]. Sections are
       aligned within their page-aligned part, which this keeps to a page at most */
    snx_elf_layout(ld.objs, count, kind_size, kind_align);
    for (i = 0; i < SNX_ELF_KINDS; ++i)
        if (kind_align[i] > loader->page) {
            snprintf(error, error_size, "a section asks for alignment above a page");
            goto done;
        }
    start = ld.arena_start + loader->used;
    kind_address[SNX_ELF_TEXT] = start;
    ld.stub_address = align_up(start + kind_size[SNX_ELF_TEXT], 8);
    text_end = ld.stub_address + STUB_SIZE * (uint64_t)ld.stub_count;
    read_only = align_up(text_end, loader->page);
    kind_address[SNX_ELF_RODATA] = read_only;
    ld.got_address = align_up(read_only + kind_size[SNX_ELF_RODATA], 8);
    writable = align_up(ld.got_address + 8 * (uint64_t)ld.got_count, loader->page);
    kind_address[SNX_ELF_DATA] = writable;
    kind_address[SNX_ELF_BSS] = align_up(writable + kind_size[SNX_ELF_DATA], kind_align[SNX_ELF_BSS]);
    end = align_up(kind_address[SNX_ELF_BSS] + kind_size[SNX_ELF_BSS], loader->page);
    if (end > ld.arena_end) {
        snprintf(error, error_size, "the code arena is full (%lu KiB)", (unsigned long)(loader->reserved >> 10));
        goto done;
    }
    /* written while read-write. A failed load leaves its pages so, for the next load to write over: no address of
       theirs went out, so nothing runs there */
    if (end > start) {
        if (snx_vm_protect(at(loader, &ld, start), (size_t)(end - start), SNX_VM_READ_WRITE)) {
            snprintf(error, error_size, "cannot commit %lu KiB of code memory", (unsigned long)((end - start) >> 10));
            goto done;
        }
        memset(at(loader, &ld, start), 0xCC, (size_t)(read_only - start)); /* int3 between functions and stubs */
        memset(at(loader, &ld, read_only), 0, (size_t)(end - read_only));
    }
    for (i = 0; i < count; ++i) {
        SnxElfObject* o = &ld.objs[i];
        for (j = 0; j < o->eh->e_shnum; ++j) {
            if (o->kind[j] < 0) continue;
            o->address[j] = kind_address[o->kind[j]] + o->position[j];
            if (o->kind[j] != SNX_ELF_BSS && o->sh[j].sh_size)
                memcpy(at(loader, &ld, o->address[j]), o->input->data + o->sh[j].sh_offset, o->sh[j].sh_size);
        }
    }
    if (relocate(loader, &ld, error, error_size)) goto done;
    /* executable before any address goes out; the GOT read-only as well, as after a dynamic loader's RELRO */
    if (read_only > start && snx_vm_protect(at(loader, &ld, start), (size_t)(read_only - start), SNX_VM_READ_EXECUTE)) {
        snprintf(error, error_size, "cannot make the loaded code executable");
        goto done;
    }
    if (writable > read_only &&
        snx_vm_protect(at(loader, &ld, read_only), (size_t)(writable - read_only), SNX_VM_READ)) {
        snprintf(error, error_size, "cannot make the loaded data read-only");
        goto done;
    }
    if (text_end > start) snx_vm_flush_code(at(loader, &ld, start), (size_t)(text_end - start));
    for (i = 0; i < name_count; ++i)
        if (snx_link_find(&ld.globals, names[i], &found))
            addresses[i] = snx_elf_symbol_address(ld.objs, found >> 32, found & 0xFFFFFFFFu, &absolute);
        else
            snx_code_lookup(loader, names[i], &addresses[i]);
    for (i = 0; i < ld.globals.capacity; ++i)
        if (ld.globals.keys[i])
            snx_link_insert(&loader->symbols, ld.globals.keys[i],
                            (size_t)snx_elf_symbol_address(ld.objs, ld.globals.values[i] >> 32,
                                                           ld.globals.values[i] & 0xFFFFFFFFu, &absolute));
    loader->used = (size_t)(end - ld.arena_start);
    rc = 0;
done:
    for (i = 0; i < count; ++i) snx_elf_free(&ld.objs[i]);
    free(ld.objs);
    snx_link_names_free(&ld.globals);
    snx_link_names_free(&ld.got);
    snx_link_names_free(&ld.stubs);
    return rc;
}
#else
int snx_code_load(SnxCodeLoader* loader, const SnxLinkInput* inputs, size_t count, const char* const* names,
                  size_t name_count, SnxU64* addresses, char* error, size_t error_size) {
    (void)loader;
    (void)inputs;
    (void)count;
    (void)names;
    (void)name_count;
    (void)addresses;
    snprintf(error, error_size, "the JIT loader reads ELF objects, and Windows' are COFF");
    return -1;
}
#endif
