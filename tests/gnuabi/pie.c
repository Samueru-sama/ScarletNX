/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright 2026 the ScarletNX contributors */
/* Relocation-free static PIE: all global references are PC-relative. */
#include "guest.h"
static volatile u64 data = 0x1020304050607080UL;
static volatile u64 bss[1537];
int guest_main(u64* stack) {
    u64 result = data;
    for (u64 i = 0; i != 1537; ++i) {
        if (bss[i]) return 1;
        bss[i] = result ^ (i * 0x100000001UL);
        result += bss[i];
    }
    if (((u64)stack & 15) || stack[0] != 1) return 2;
    emit_hex(result);
    return 0;
}
