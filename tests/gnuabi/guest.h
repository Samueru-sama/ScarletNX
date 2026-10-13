/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright 2026 the ScarletNX contributors */
#ifndef SNX_GNUABI_TEST_GUEST_H
#define SNX_GNUABI_TEST_GUEST_H
typedef unsigned long u64;
typedef long s64;
typedef unsigned int u32;
typedef unsigned char u8;

static inline s64 syscall6(s64 number, u64 a, u64 b, u64 c, u64 d, u64 e, u64 f) {
    register u64 x0 __asm__("x0") = a;
    register u64 x1 __asm__("x1") = b;
    register u64 x2 __asm__("x2") = c;
    register u64 x3 __asm__("x3") = d;
    register u64 x4 __asm__("x4") = e;
    register u64 x5 __asm__("x5") = f;
    register s64 x8 __asm__("x8") = number;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x8) : "memory", "cc");
    return (s64)x0;
}
static inline s64 call(s64 number, u64 a, u64 b, u64 c) { return syscall6(number, a, b, c, 0, 0, 0); }
static inline void emit_hex(u64 value) {
    char buffer[17];
    const char* digits = "0123456789abcdef";
    for (int i = 0; i != 16; ++i) buffer[i] = digits[(value >> (60 - i * 4)) & 15];
    buffer[16] = '\n';
    call(64, 1, (u64)buffer, sizeof(buffer));
}
#endif
