/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* ELF64 x86-64 relocatable objects, as LLVM and Clang write them: what the AOT linker (link_elf.c) and the JIT loader
   (code_loader.c) both do with one. Both take the same sections, resolve symbols by the same rules and write every
   relocation through snx_elf_relocate, so code loaded into the process runs as the same code linked into a module.
   Not on Windows hosts, which have no <elf.h> and whose objects are COFF (link_coff.c) */
#ifndef SNX_RECOMPILER_LLVM_ELF_OBJECT_H
#define SNX_RECOMPILER_LLVM_ELF_OBJECT_H
#ifndef _WIN32
#include "link.h"
#include <elf.h>
#include <stdint.h>

/* names newer than some systems' <elf.h> (FreeBSD's calls the unwind section SHT_AMD64_UNWIND) */
#ifndef SHT_X86_64_UNWIND
#define SHT_X86_64_UNWIND 0x70000001
#endif
#ifndef R_X86_64_GOTPCRELX
#define R_X86_64_GOTPCRELX 41
#endif
#ifndef R_X86_64_REX_GOTPCRELX
#define R_X86_64_REX_GOTPCRELX 42
#endif
#ifndef R_X86_64_PLT32
#define R_X86_64_PLT32 4
#endif

/* where an allocated section goes, by its flags: the order of the AOT module's segments */
enum { SNX_ELF_RODATA, SNX_ELF_TEXT, SNX_ELF_DATA, SNX_ELF_BSS, SNX_ELF_KINDS };

/* A view of one input object. The section arrays are one block, freed by snx_elf_free */
typedef struct {
    const SnxLinkInput* input;
    const Elf64_Ehdr* eh;
    const Elf64_Shdr* sh;
    const Elf64_Sym* syms;
    size_t nsyms;
    const char* strtab;
    int* kind;          /* per section: SNX_ELF_*, -1 when not loaded */
    uint64_t* address;  /* per section: final address */
    uint64_t* position; /* per section: offset inside its kind */
} SnxElfObject;

/* Checks the object and classifies its sections. 0, or -1 with the reason in error */
int snx_elf_parse(SnxElfObject* o, const SnxLinkInput* input, char* error, size_t error_size);
void snx_elf_free(SnxElfObject* o);
const char* snx_elf_symbol_name(const SnxElfObject* o, const Elf64_Sym* s);
const char* snx_elf_section_name(const SnxElfObject* o, size_t index);

/* Every global and weak definition of the set into globals, name -> (object << 32 | symbol index). A strong definition
   wins over weak ones, two strong ones are an error. Names in loaded (NULL for none: symbols outside the set, already
   defined) are skipped when the definition is weak and an error when it is strong */
int snx_elf_collect_globals(const SnxElfObject* objs, size_t count, const SnxLinkNames* loaded, SnxLinkNames* globals,
                            char* error, size_t error_size);

/* Where object's symbol index is defined: SNX_ELF_HERE, with the defining object and symbol in the set (itself for a
   local or defined symbol), or SNX_ELF_ELSEWHERE for an undefined one no object of the set defines, which the caller
   resolves by its name or reports. -1 for a bad index */
enum { SNX_ELF_HERE, SNX_ELF_ELSEWHERE };
int snx_elf_definition(const SnxElfObject* objs, const SnxLinkNames* globals, size_t object, size_t index,
                       size_t* def_object, size_t* def_index, char* error, size_t error_size);

/* The address of a definition in the set once its sections have addresses; *absolute for an SHN_ABS value */
uint64_t snx_elf_symbol_address(const SnxElfObject* objs, size_t object, size_t index, int* absolute);

/* 1 for the relocations that go through a GOT slot */
int snx_elf_got_relocation(unsigned type);

/* Positions of the loaded sections inside their kinds, in object and section order, and each kind's size and
   alignment */
void snx_elf_layout(SnxElfObject* objs, size_t count, uint64_t* kind_size, uint64_t* kind_align);

/* Writes relocation type at p, the place at address P with room bytes left in its section: S the symbol's address, A
   the addend, got the GOT's address and slot the address of the symbol's GOT slot (GOT relocations only). 0, or
   SNX_LINK_OUTSIDE when the field does not fit in the section, SNX_LINK_RANGE when a 32-bit field cannot hold the
   value, SNX_LINK_UNSUPPORTED for a type neither linker handles; nothing is written then */
int snx_elf_relocate(unsigned char* p, size_t room, unsigned type, uint64_t S, int64_t A, uint64_t P, uint64_t got,
                     uint64_t slot);

#endif
#endif
