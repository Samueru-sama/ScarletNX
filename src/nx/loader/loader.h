/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Derived from SwitchRecomp nxloader.py, Copyright 2026 MZ Labs, Apache-2.0.
   Retain LICENSE.apache and NOTICE; the fork's changes are GPL-3.0-or-later. */
#ifndef SNX_TOOL_LOADER_H
#define SNX_TOOL_LOADER_H

#include "common.h"
#include "image.h"
#include <stddef.h>

#define SNX_CODE_BASE 0x08000000UL
#define SNX_PAGE_SIZE 0x1000UL

typedef struct {
    SnxU64 value, size;
    char* name;
    unsigned type;
} SnxSymbol;

typedef struct {
    SnxU64 off, dynamic, bss_start, bss_end, ehf_start, ehf_end;
} SnxMod0;

typedef struct {
    char *name, *path;
    unsigned char* image;
    size_t image_size;
    SnxU32 text_off, text_size, ro_off, ro_size, data_off, data_size, bss;
    unsigned char build_id[32];
    SnxU64 base, size;
    SnxSymbol* symbols;
    size_t symbol_count;
    SnxU64 *functions, *relptrs;
    size_t function_count, relptr_count;
    SnxFde* fde;
    size_t fde_count;
    char** imports;
    size_t import_count;
    int is_nro, has_mod0;
    SnxMod0 mod0;
    char* eh_error;
    /* Asset and RomFS pointers alias the allocation owned by image. */
    const unsigned char *nro_asset, *romfs_data;
    size_t nro_asset_size, romfs_size;
} SnxModuleImage;

typedef struct {
    SnxU64 title_id;
    SnxU32 stack;
    unsigned prio, core, aspace;
    int is64;
    char name[17];
} SnxNpdm;

typedef struct {
    SnxModuleImage* modules;
    size_t module_count;
    unsigned char* npdm_data;
    size_t npdm_size;
    SnxNpdm npdm;
    char *entry_path, *exefs_path, *root_path, *romfs_path;
    /* Embedded NRO RomFS aliases modules[0].image and is valid until snx_free_program. */
    const unsigned char* romfs_data;
    size_t romfs_size;
} SnxProgram;

/* Return 0 on success, -1 with a bounded error string on failure. Output structs are zeroed on failure.
   Offsets in symbols/functions/relptrs/FDE/MOD0 are module-relative. Functions and relptrs are sorted, unique.
   Malformed unwind metadata preserves earlier FDEs and reports eh_error; other malformed structures fail loading. */
int snx_load_program(const char* entry, SnxProgram* program, char* error, size_t error_size);
void snx_free_program(SnxProgram* program);
int snx_load_nso(const char* path, const char* name, SnxModuleImage* module, char* error, size_t error_size);
int snx_load_nro(const char* path, SnxModuleImage* module, char* error, size_t error_size);
void snx_free_module(SnxModuleImage* module);
int snx_parse_npdm(const unsigned char* data, size_t size, SnxNpdm* npdm, char* error, size_t error_size);

#endif
