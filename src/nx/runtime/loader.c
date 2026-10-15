/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* NSO/NRO loading. The code segment has to match what we recompiled byte for byte. */
#include "runtime/loader.h"
#include "runtime/main.h"
#include "hle/filesystem/fs.h"
#include "memory/mem.h"
#include "runtime/platform/log.h"
#include "runtime/platform/plat.h"
#include <lz4.h>
#include <stdio.h>
#include <string.h>

u64 g_code_lo, g_code_hi;

static u32 nro_u32(const u8* p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }

static int load_nro(const SnxModInfo* mi, const u8* f, u64 fsz, u8* dst) {
    /* Layout and compression flag: Atmosphere ro::NroHeader and ro::ValidateNro.
       Assets after Size are not mapped, and BSS is separate zero-filled memory. */
    if (fsz < 0x80 || memcmp(f + 0x10, "NRO0", 4)) {
        WARN("%s: missing or truncated NRO0 header\n", mi->name);
        return -1;
    }
    u64 isz = nro_u32(f + 0x18);
    u32 flags = nro_u32(f + 0x1c), bss = nro_u32(f + 0x38);
    u32 seg[3][2];
    for (int i = 0; i < 3; i++) {
        seg[i][0] = nro_u32(f + 0x20 + i * 8);
        seg[i][1] = nro_u32(f + 0x24 + i * 8);
    }
    if ((flags & 2) || isz < 0x80 || isz > fsz || isz > mi->size || bss > mi->size - isz || seg[0][0] ||
        seg[0][1] < 0x80 || (u64)seg[0][1] != seg[1][0] || (u64)seg[1][0] + seg[1][1] != seg[2][0] ||
        (u64)seg[2][0] + seg[2][1] != isz) {
        WARN("%s: invalid or unsupported NRO layout\n", mi->name);
        return -1;
    }
    if (memcmp(f + 0x40, mi->build_id, 32) || seg[0][0] != mi->text_off || seg[0][1] != mi->text_size ||
        seg[1][0] != mi->ro_off || seg[1][1] != mi->ro_size || seg[2][0] != mi->data_off ||
        seg[2][1] != mi->data_size || bss != mi->bss_size) {
        WARN("WARNING: module %s is not the NRO that was recompiled (different build id or layout).\n", mi->name);
        return -2;
    }
    memcpy(dst, f, (size_t)isz);
    memset(dst + isz, 0, (size_t)(mi->size - isz));
    return 0;
}

static int load_one(const SnxModInfo* mi) {
    char p[1200];
    /* mapped, not read: the NSOs are tens of MB and only copied out once. The mapping ends at the file's end, so every
       read below is checked against fsz */
    PFileMap fm;
    int found = 0;
    const char* dirs[] = {"exefs/", ""};
    for (int k = 0; k < 2 && !found; k++) {
        snprintf(p, sizeof p, "%s%s%s", dirs[k], mi->name, mi->is_nro ? ".nro" : "");
        found = !fs_map_file(p, &fm);
    }
    if (!found) {
        WARN("Cannot find module %s in %sexefs/\n", mi->name, g_datadir);
        return -1;
    }
    const u8* f = fm.data;
    u64 fsz = fm.size;
    u64 size = mi->size;
    if (mem_map(mi->base, size, MS_CODE, PERM_RW)) {
        WARN("could not map %s at %llx\n", mi->name, (unsigned long long)mi->base);
        plat_file_unmap(&fm);
        return -1;
    }
    u8* dst = GP(mi->base);
    if (mi->is_nro) {
        int result = load_nro(mi, f, fsz, dst);
        if (result) {
            plat_file_unmap(&fm);
            return result;
        }
    } else if (fsz >= 0x100 && !memcmp(f, "NSO0", 4)) {
        u32 flags;
        memcpy(&flags, f + 0xC, 4);
        u32 seg[3][3];
        for (int i = 0; i < 3; i++) memcpy(seg[i], f + 0x10 + i * 0x10, 12);
        u32 fs[3];
        memcpy(fs, f + 0x60, 12);
        if (memcmp(f + 0x40, mi->build_id, 32)) {
            WARN("WARNING: module %s is not the one that was recompiled (different build id).\n", mi->name);
            plat_file_unmap(&fm);
            return -2;
        }
        for (int i = 0; i < 3; i++) { /* each segment inside the file, and inside the module's mapping */
            u32 stored = (flags & (1u << i)) ? fs[i] : seg[i][2];
            if (seg[i][0] > fsz || stored > fsz - seg[i][0] || seg[i][1] > size || seg[i][2] > size - seg[i][1]) {
                WARN("%s: segment %d lies outside the file or the module\n", mi->name, i);
                plat_file_unmap(&fm);
                return -1;
            }
            if (flags & (1u << i)) {
                /* LZ4_decompress_safe stays inside both buffers: a damaged file fails instead of reading past it */
                if (fs[i] > LZ4_MAX_INPUT_SIZE || seg[i][2] > LZ4_MAX_INPUT_SIZE ||
                    LZ4_decompress_safe((const char*)f + seg[i][0], (char*)dst + seg[i][1], (int)fs[i],
                                        (int)seg[i][2]) != (int)seg[i][2]) {
                    WARN("LZ4 failed in %s\n", mi->name);
                    plat_file_unmap(&fm);
                    return -1;
                }
            } else
                memcpy(dst + seg[i][1], f + seg[i][0], seg[i][2]);
        }
    } else {
        WARN("%s: unknown format\n", p);
        plat_file_unmap(&fm);
        return -1;
    }
    plat_file_unmap(&fm);
    /* .text RX, .rodata R, .data/.bss RW */
    u64 t0 = mi->base + mi->text_off, r0 = mi->base + mi->ro_off, d0 = mi->base + mi->data_off;
    mem_set_state(t0, ((mi->text_size + 0xFFF) & ~0xFFFu), MS_CODE, PERM_RX);
    mem_set_state(r0, ((mi->ro_size + 0xFFF) & ~0xFFFu), MS_CODE, PERM_R);
    mem_set_state(d0, size - (d0 - mi->base), MS_CODEDATA, PERM_RW);
    /* Account the loaded image, including data and BSS, but not the unmapped inter-module guard pages. */
    g_code_memory += (size + 0xFFF) & ~0xFFFull;
    LOG("module %-8s at %010llx (%.1f MB)\n", mi->name, (unsigned long long)mi->base, (double)size / 1048576.0);
    return 0;
}

int loader_load(void) {
    for (u32 i = 0; i < g_mod->nmods; i++)
        if (load_one(&g_mod->mods[i])) return -1;
    return 0;
}
