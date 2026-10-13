/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* the file system services and the game's romfs (fs.c) */
#ifndef SNX_FS_H
#define SNX_FS_H
#include "cpu/aarch64/cpu.h"
#include "runtime/platform/plat.h"

/* Index the game data directory once, before loading guest files. */
void fs_init(void);
FILE* fs_open_read(const char* path, u64* size);
int fs_map_file(const char* path, PFileMap* map);
u8* fs_read_file(const char* path, u64* size);
int fs_add_virtual_file(const char* path, const void* data, u64 size);
void romfs_init(void);

#endif
