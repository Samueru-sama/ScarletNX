/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The JIT's loader (code_loader.h): a code arena, and the LLVM backend's ELF objects linked into it */
#ifdef __linux__
#define _GNU_SOURCE /* memfd_create */
#else
#define _POSIX_C_SOURCE 200809L /* snprintf */
#endif
#include "code_loader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

/* the table keeps addresses in SnxLinkNames' size_t values */
typedef char snx_code_loader_needs_64_bits[sizeof(size_t) >= 8 ? 1 : -1];

/* the arena's regions, in address order (code_loader.h) */
enum { CODE, RODATA, DATA, REGIONS };

typedef struct {
    size_t start, size; /* in the arena */
    size_t used;        /* bytes from start that loads hold */
    /* where the loader writes the region's bytes: its read-write view (code, read-only data) or the region itself
       (data). NULL where nothing is loaded (Windows) */
    unsigned char* write;
} Region;

struct SnxCodeLoader {
    unsigned char* base; /* the arena */
    size_t reserved, page;
    Region region[REGIONS];
    size_t committed;     /* bytes from the data region's start that are read-write */
    SnxLinkNames symbols; /* name -> address: the globals of earlier loads and the defined names */
};

#ifndef _WIN32
/* Shared memory of size bytes, mapped at at (in the arena's reservation) readable, and executable when asked, and once
   more read-write where the system puts it: that view, or NULL with the reason in error */
static unsigned char* map_views(unsigned char* at, size_t size, int executable, char* error, size_t error_size) {
    void *view, *write = MAP_FAILED;
    int fd;
#if defined(__linux__)
    fd = memfd_create("snx-jit-code", MFD_CLOEXEC);
#elif defined(SHM_ANON)
    fd = shm_open(SHM_ANON, O_RDWR | O_CLOEXEC, 0600);
#else
    fd = -1;
#endif
    if (fd < 0) {
        snprintf(error, error_size, "no anonymous shared memory for the code arena");
        return NULL;
    }
    if (!ftruncate(fd, (off_t)size)) {
        view = mmap(at, size, PROT_READ | (executable ? PROT_EXEC : 0), MAP_SHARED | MAP_FIXED, fd, 0);
        if (view == (void*)at) write = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    }
    close(fd); /* the mappings keep the memory */
    if (write == MAP_FAILED) {
        snprintf(error, error_size, "cannot map %lu MiB of code memory twice", (unsigned long)(size >> 20));
        return NULL;
    }
    return (unsigned char*)write;
}
#endif

SnxCodeLoader* snx_code_loader_new(size_t reserve, char* error, size_t error_size) {
    SnxCodeLoader* loader;
    size_t page = snx_vm_page_size();
    int i;
    if (!reserve) reserve = SNX_CODE_ARENA_DEFAULT;
    if (reserve <= (size_t)1 << 31) reserve = (reserve + 8 * page - 1) / (8 * page) * (8 * page); /* whole pages */
    if (reserve > (size_t)1 << 31) {
        snprintf(error, error_size, "a code arena holds at most 2 GiB, so 32-bit displacements reach across it");
        return NULL;
    }
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
    /* code is most of what a unit holds. Read-only data is its jump tables, constants, function table and GOT, and
       generated code writes no data of its own (nor does the runtime library) */
    loader->region[CODE].size = reserve / 8 * 5;
    loader->region[RODATA].start = loader->region[CODE].size;
    loader->region[RODATA].size = reserve / 8 * 2;
    loader->region[DATA].start = loader->region[RODATA].start + loader->region[RODATA].size;
    loader->region[DATA].size = reserve - loader->region[DATA].start;
#ifndef _WIN32
    for (i = CODE; i <= RODATA; ++i) {
        loader->region[i].write =
            map_views(loader->base + loader->region[i].start, loader->region[i].size, i == CODE, error, error_size);
        if (!loader->region[i].write) {
            snx_code_loader_free(loader);
            return NULL;
        }
    }
    loader->region[DATA].write = loader->base + loader->region[DATA].start;
#else
    (void)i;
#endif
    return loader;
}

void snx_code_loader_free(SnxCodeLoader* loader) {
    if (!loader) return;
#ifndef _WIN32
    if (loader->region[CODE].write) munmap(loader->region[CODE].write, loader->region[CODE].size);
    if (loader->region[RODATA].write) munmap(loader->region[RODATA].write, loader->region[RODATA].size);
#endif
    snx_vm_release(loader->base, loader->reserved); /* the arena's views with it */
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

size_t snx_code_used(const SnxCodeLoader* loader) {
    return loader->region[CODE].used + loader->region[RODATA].used + loader->region[DATA].used;
}

#ifndef _WIN32
#include "elf_object.h"

/* jmp *slot(%rip), and two int3 to the next stub: the call of a table symbol beyond 32-bit reach, through its GOT
   slot */
#define STUB_SIZE 8
/* where each load's code starts: a line no running code shares, so writing it never touches a line another core may
   be executing from */
#define CODE_LINE 64

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

/* where the loader writes the byte the arena has at address: through its region's read-write view */
static unsigned char* at(const SnxCodeLoader* loader, uint64_t address) {
    size_t offset = (size_t)(address - (uint64_t)(size_t)loader->base);
    const Region* r = &loader->region[offset >= loader->region[DATA].start     ? DATA
                                      : offset >= loader->region[RODATA].start ? RODATA
                                                                               : CODE];
    return r->write + (offset - r->start);
}

/* the arena address of the next free byte of a region */
static uint64_t next_free(const SnxCodeLoader* loader, int region) {
    return (uint64_t)(size_t)loader->base + loader->region[region].start + loader->region[region].used;
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
    snx_link_put64(at(loader, address), t->address);
    return address;
}

/* t's stub, written to jump through its GOT slot: the stub's address */
static uint64_t stub(const SnxCodeLoader* loader, const Load* ld, const Target* t) {
    size_t index = 0;
    uint64_t address, slot = got_slot(loader, ld, t);
    unsigned char* p;
    snx_link_find(&ld->stubs, t->key, &index);
    address = ld->stub_address + STUB_SIZE * (uint64_t)index;
    p = at(loader, address);
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
                    p = at(loader, P);
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
    static const char* const region_names[REGIONS] = {"code", "read-only data", "data"};
    Load ld;
    uint64_t kind_size[SNX_ELF_KINDS], kind_align[SNX_ELF_KINDS], kind_address[SNX_ELF_KINDS];
    uint64_t start[REGIONS], end[REGIONS], limit, need;
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
    /* the unit's parts, each at its region's next free byte: [code, stubs] [read-only data, GOT] [data, bss] */
    snx_elf_layout(ld.objs, count, kind_size, kind_align);
    start[CODE] =
        align_up(next_free(loader, CODE), kind_align[SNX_ELF_TEXT] > CODE_LINE ? kind_align[SNX_ELF_TEXT] : CODE_LINE);
    kind_address[SNX_ELF_TEXT] = start[CODE];
    ld.stub_address = align_up(start[CODE] + kind_size[SNX_ELF_TEXT], 8);
    end[CODE] = ld.stub_address + STUB_SIZE * (uint64_t)ld.stub_count;
    start[RODATA] = align_up(next_free(loader, RODATA), kind_align[SNX_ELF_RODATA]);
    kind_address[SNX_ELF_RODATA] = start[RODATA];
    ld.got_address = align_up(start[RODATA] + kind_size[SNX_ELF_RODATA], 8);
    end[RODATA] = ld.got_address + 8 * (uint64_t)ld.got_count;
    start[DATA] = align_up(next_free(loader, DATA), kind_align[SNX_ELF_DATA]);
    kind_address[SNX_ELF_DATA] = start[DATA];
    kind_address[SNX_ELF_BSS] = align_up(start[DATA] + kind_size[SNX_ELF_DATA], kind_align[SNX_ELF_BSS]);
    end[DATA] = kind_address[SNX_ELF_BSS] + kind_size[SNX_ELF_BSS];
    for (i = 0; i < REGIONS; ++i) {
        limit = ld.arena_start + loader->region[i].start + loader->region[i].size;
        if (end[i] > limit) {
            snprintf(error, error_size, "the code arena's %s is full (%lu KiB)", region_names[i],
                     (unsigned long)(loader->region[i].size >> 10));
            goto done;
        }
    }
    /* data pages become read-write as loads reach them; pages that are stay so, used or not */
    need = align_up(end[DATA] - (ld.arena_start + loader->region[DATA].start), loader->page);
    if (need > loader->committed) {
        if (snx_vm_protect(loader->region[DATA].write + loader->committed, (size_t)(need - loader->committed),
                           SNX_VM_READ_WRITE)) {
            snprintf(error, error_size, "cannot commit %lu KiB of data memory",
                     (unsigned long)((need - loader->committed) >> 10));
            goto done;
        }
        loader->committed = (size_t)need;
    }
    /* Written through the read-write views, from each region's first free byte: a failed load leaves its bytes for the
       next to write over, since no address of theirs went out and nothing runs there. int3 between functions */
    memset(at(loader, next_free(loader, CODE)), 0xCC, (size_t)(end[CODE] - next_free(loader, CODE)));
    memset(at(loader, next_free(loader, RODATA)), 0, (size_t)(end[RODATA] - next_free(loader, RODATA)));
    memset(at(loader, next_free(loader, DATA)), 0, (size_t)(end[DATA] - next_free(loader, DATA)));
    for (i = 0; i < count; ++i) {
        SnxElfObject* o = &ld.objs[i];
        for (j = 0; j < o->eh->e_shnum; ++j) {
            if (o->kind[j] < 0) continue;
            o->address[j] = kind_address[o->kind[j]] + o->position[j];
            if (o->kind[j] != SNX_ELF_BSS && o->sh[j].sh_size)
                memcpy(at(loader, o->address[j]), o->input->data + o->sh[j].sh_offset, o->sh[j].sh_size);
        }
    }
    if (relocate(loader, &ld, error, error_size)) goto done;
    /* the code is complete: visible to instruction fetch before any address goes out (code_loader.h) */
    if (end[CODE] > start[CODE]) snx_vm_flush_code((void*)(size_t)start[CODE], (size_t)(end[CODE] - start[CODE]));
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
    /* a region the unit put nothing into keeps its next free byte, so an empty load uses nothing */
    for (i = 0; i < REGIONS; ++i)
        if (end[i] > start[i]) loader->region[i].used = (size_t)(end[i] - ld.arena_start - loader->region[i].start);
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
