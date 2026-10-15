/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The JIT's loader: the LLVM backend's relocatable objects (snx_ir_object's, the runtime library's) placed in this
   process's memory and linked there, where the AOT path links them into a module file for dlopen (link_elf.c). The
   objects are read and relocated by elf_object.c, as link_elf.c reads and relocates them.

   A loader owns one code arena: address space reserved once and given back only when the loader is freed. It is at
   most 2 GiB, so a 32-bit displacement reaches from any byte of it to any other: code loaded later calls the runtime
   library loaded first directly. The arena is three regions, each filled front to back with the loads packed one
   after another (a load's code from a new cache line, its other parts as their sections align):
     [code, stubs: read-execute] [read-only data, GOT: read-only] [data, bss: read-write]
   The first two are shared memory mapped twice: the region in the arena, which code runs from and which never
   becomes writable, and a read-write view elsewhere that the loader writes through, with every address computed for
   the arena. So a load goes next to code other threads are running, on the same pages, with no page writable and
   executable in one view (W^X) and no protection change per load. The data region is private memory, made read-write
   a page at a time as loads reach it.

   A load's symbols resolve to the load's own definitions first, then to the loader's table: the globals of earlier
   loads and the names given to snx_code_define (the runtime's functions and data, anywhere in the address space). A
   call to a table symbol out of a 32-bit displacement's reach goes through a stub in the load, and a GOT reference gets
   a slot in the load's read-only GOT, which holds the full address.

   Threads: one call at a time per loader, which the caller serializes. Code from earlier loads keeps running during a
   load, which writes only bytes no earlier load uses, and an address comes out of snx_code_load only once its code is
   written and the instruction cache flushed: the caller can hand it to other threads (with release ordering) as is.
   x86-64 keeps instruction fetch coherent with stores from every core, checked on physical addresses (Intel SDM vol.
   3A, 11.6), so through the other view too, and no thread can jump to the new bytes before it loads their address.
   An AArch64 host would need more for code on pages that already ran: an ISB in each thread after it loads the
   address, or membarrier(MEMBARRIER_CMD_PRIVATE_EXPEDITED_SYNC_CORE) before the address goes out.

   x86-64 ELF hosts with memfd_create (Linux) or SHM_ANON (FreeBSD). On Windows, where the objects are COFF,
   snx_code_load refuses for now. The views would be a pagefile-backed section (CreateFileMapping, with
   PAGE_EXECUTE_READWRITE as its maximum protection) mapped twice: with MapViewOfFile3 and PAGE_EXECUTE_READ into a
   placeholder cut out of the arena's reservation (VirtualAlloc2 with MEM_RESERVE_PLACEHOLDER, VirtualFree with
   MEM_PRESERVE_PLACEHOLDER, MEM_REPLACE_PLACEHOLDER), and with MapViewOfFile and PAGE_READWRITE anywhere */
#ifndef SNX_RECOMPILER_LLVM_CODE_LOADER_H
#define SNX_RECOMPILER_LLVM_CODE_LOADER_H
#include "common.h"
#include "link.h"

typedef struct SnxCodeLoader SnxCodeLoader;

/* the arena's size when snx_code_loader_new gets 0 */
#define SNX_CODE_ARENA_DEFAULT ((size_t)1 << 30)

/* Reserves an arena of reserve bytes (0 for SNX_CODE_ARENA_DEFAULT, at most 2 GiB), five eighths of it code, two
   eighths read-only data and the rest data. NULL with the reason in error */
SnxCodeLoader* snx_code_loader_new(size_t reserve, char* error, size_t error_size);
/* Gives the arena back: nothing may run the loaded code any more */
void snx_code_loader_free(SnxCodeLoader* loader);
/* Defines name at address for the loads that follow. 0, or -1 when the table has the name already */
int snx_code_define(SnxCodeLoader* loader, const char* name, SnxU64 address);
/* 1 and *address when name is in the table: an earlier load's global or a defined name */
int snx_code_lookup(const SnxCodeLoader* loader, const char* name, SnxU64* address);
/* Loads count objects as one unit: they resolve each other's globals, which join the table for later loads.
   addresses[i] receives names[i]'s address, a global of the unit or of the table. 0, or -1 with the reason in error
   and the loader as it was before the call */
int snx_code_load(SnxCodeLoader* loader, const SnxLinkInput* inputs, size_t count, const char* const* names,
                  size_t name_count, SnxU64* addresses, char* error, size_t error_size);
/* the bytes of the arena loads use, the three regions together, alignment padding included */
size_t snx_code_used(const SnxCodeLoader* loader);

#endif
