/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* pl: the shared fonts */
#include "nx/runtime/main.h"
#include "nx/hle/filesystem/fs.h"
#include "nx/hle/services/services.h"
#include "nx/hle/kernel/ipc.h"
#include "hle/services/pl.swipc.inc"
#include "nx/hle/kernel/kernel.h"
#include "nx/memory/mem.h"
#include "nx/runtime/platform/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* pl:u shared fonts. We don't ship the console's: use GAME_data/fonts/<Name>.bfttf|.ttf if there, else the stand-ins
   compiled in (cmake/shared_fonts.cmake), as Eden carries them: Open Sans for every slot but FontNintendoExtended,
   which takes the controller button glyphs yuzu drew. The shared memory holds each font as plain TTF behind the 8
   byte obfuscated header the SDK reads its size from (a .bfttf is decrypted into it). */
static const char* g_font_names[6] = {
    "FontStandard", "FontChineseSimplified", "FontExtendedChineseSimplified", "FontChineseTraditional",
    "FontKorean",   "FontNintendoExtended"};
static struct {
    u32 font_off[6], font_size[6];
    KShmem* font_shm;
    PMutex font_lock; /* the first request fills the shared memory, the others wait for it */
} pl;
static u8* read_indexed_font(const char* path, u64* size) {
    u8* d = fs_read_file(path, size);
    if (d && !*size) {
        free(d);
        return NULL;
    }
    return d;
}
static u32 bswap32_(u32 v) { return (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24); }
extern const unsigned char snx_font_standard[], snx_font_extended[];
extern const size_t snx_font_standard_size, snx_font_extended_size;
static void fonts_fill(u8* shm, u64 cap) {
    const u32 RESULT = 0x7f9a0218u, MAGIC = 0x36f81a1eu;
    u32 key = bswap32_(RESULT ^ MAGIC);
    u64 off = 0;
    u32 std_off = 0, std_size = 0; /* the stand-in Open Sans, once in the shared memory for every slot it backs */
    for (int i = 0; i < 6; i++) {
        char p[1200];
        u64 sz = 0;
        u8* d = NULL;
        int is_bfttf = 0;
        snprintf(p, sizeof p, "fonts/%s.bfttf", g_font_names[i]);
        d = read_indexed_font(p, &sz);
        if (d) is_bfttf = 1;
        if (!d) {
            snprintf(p, sizeof p, "fonts/%s.ttf", g_font_names[i]);
            d = read_indexed_font(p, &sz);
        }
        int builtin = !d, ext = i == 5;
        if (builtin) {
            if (!ext && std_size) {
                pl.font_off[i] = std_off;
                pl.font_size[i] = std_size;
                continue;
            }
            d = (u8*)(ext ? snx_font_extended : snx_font_standard);
            sz = ext ? snx_font_extended_size : snx_font_standard_size;
        } else
            LOG("pl: %s <- %s\n", g_font_names[i], p);
        if (is_bfttf && sz < 8) { /* not even the header */
            WARN("pl: %s is not a BFTTF font\n", p);
            free(d);
            continue;
        }
        u64 words = (sz + 3) / 4;
        if (off + 8 + words * 4 > cap) {
            WARN("pl: font %s does not fit\n", g_font_names[i]);
            if (!builtin) free(d);
            continue;
        }
        u32 h0 = bswap32_(MAGIC), h1 = bswap32_((u32)(words * 4)) ^ key;
        memcpy(shm + off, &h0, 4);
        memcpy(shm + off + 4, &h1, 4);
        if (is_bfttf) { /* bfttf -> decrypt, shmem holds plain ttf */
            u32 w0, w1;
            memcpy(&w0, d, 4);
            memcpy(&w1, d + 4, 4);
            u32 k2 = w0 ^ RESULT;
            u32 tsz = bswap32_(w1 ^ k2);
            if (tsz > sz - 8) tsz = (u32)(sz - 8);
            /* word by word; a last partial word reads only the bytes the file has */
            for (u64 k = 0; k + 8 < sz; k += 4) {
                u32 w = 0;
                memcpy(&w, d + 8 + k, sz - 8 - k < 4 ? sz - 8 - k : 4);
                w = bswap32_(w ^ k2);
                memcpy(shm + off + 8 + k, &w, 4);
            }
            pl.font_off[i] = (u32)off + 8;
            pl.font_size[i] = tsz;
        } else {
            memcpy(shm + off + 8, d, sz);
            pl.font_off[i] = (u32)off + 8;
            pl.font_size[i] = (u32)sz;
        }
        if (builtin && !ext) {
            std_off = pl.font_off[i];
            std_size = pl.font_size[i];
        }
        off = (off + 8 + words * 4 + 0xF) & ~0xFull;
        if (!builtin) free(d);
    }
}
/* the font shared memory, filled the first time a game asks for anything */
static KShmem* font_shm(void) {
    pm_lock(&pl.font_lock);
    KShmem* shm = pl.font_shm;
    if (!shm) {
        /* the service keeps it for the whole run: its reference keeps it alive when the game closes its handle */
        shm = kshmem_new(0x1100000, "font");
        kobj_ref(&shm->o);
        fonts_fill(kshmem_ptr(shm), 0x1100000);
        pl.font_shm = shm;
    }
    pm_unlock(&pl.font_lock);
    return shm;
}
static u32 font_index(u32 font_type) { return font_type > 5 ? 0 : font_type; }
static int nn_pl_detail_IPlatformServiceManager_RequestLoad(SvcObj* self, u32 font_type) {
    font_shm();
    return 0;
}
static int nn_pl_detail_IPlatformServiceManager_GetLoadState(SvcObj* self, u32 font_type, u32* state) {
    font_shm();
    *state = 1; /* loaded */
    return 0;
}
static int nn_pl_detail_IPlatformServiceManager_GetSize(SvcObj* self, u32 font_type, u32* size) {
    font_shm();
    *size = pl.font_size[font_index(font_type)];
    return 0;
}
static int nn_pl_detail_IPlatformServiceManager_GetSharedMemoryAddressOffset(SvcObj* self, u32 font_type, u32* offset) {
    font_shm();
    *offset = pl.font_off[font_index(font_type)];
    return 0;
}
static int nn_pl_detail_IPlatformServiceManager_GetSharedMemoryNativeHandle(SvcObj* self, u32* shared_memory) {
    *shared_memory = h_new(&font_shm()->o);
    return 0;
}
/* every font, in index order, whatever the language */
static int fonts_in_order(IpcBuf codes, IpcBuf offsets, IpcBuf sizes, u8* loaded, u32* count) {
    font_shm();
    for (u32 i = 0; i < 6; i++) {
        if (codes.addr && (i + 1) * 4 <= codes.size) gw32(codes.addr + i * 4, i);
        if (offsets.addr && (i + 1) * 4 <= offsets.size) gw32(offsets.addr + i * 4, pl.font_off[i]);
        if (sizes.addr && (i + 1) * 4 <= sizes.size) gw32(sizes.addr + i * 4, pl.font_size[i]);
    }
    *loaded = 1;
    *count = 6;
    return 0;
}
static int nn_pl_detail_IPlatformServiceManager_GetSharedFontInOrderOfPriority(
    SvcObj* self, const nn_settings_LanguageCode* language_code, IpcBuf font_codes, IpcBuf offsets, IpcBuf sizes,
    u8* loaded, u32* count) {
    return fonts_in_order(font_codes, offsets, sizes, loaded, count);
}
static int nn_pl_detail_IPlatformServiceManager_GetSharedFontInOrderOfPriorityForSystem(
    SvcObj* self, const nn_settings_LanguageCode* language_code, IpcBuf font_codes, IpcBuf offsets, IpcBuf sizes,
    u8* loaded, u32* count) {
    return fonts_in_order(font_codes, offsets, sizes, loaded, count);
}
static SvcObj* new_pl(void) { return nn_pl_detail_IPlatformServiceManager_new(0); }
void svc_pl_init(void) {
    pm_init(&pl.font_lock);
    nn_pl_detail_IPlatformServiceManager_register(new_pl);
}
