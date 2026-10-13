/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Test-only host FP environment, matching nx/runtime/rtapi.c. Restored before
   returning to Python so guest rounding/FZ cannot contaminate the oracle. */
static u64 audit_host_save(void) {
#if defined(__x86_64__)
    u32 value;
    __asm__ __volatile__("stmxcsr %0" : "=m"(value));
    return value;
#elif defined(__aarch64__)
    u64 value;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(value));
    return value;
#else
#error "The audit harness needs the same host FP mapping as the runtime"
#endif
}
static void audit_host_restore(u64 value) {
#if defined(__x86_64__)
    u32 mx = (u32)value;
    __asm__ __volatile__("ldmxcsr %0" : : "m"(mx));
#else
    __asm__ __volatile__("msr fpcr, %0" : : "r"(value));
#endif
}
static void audit_fpcr(CPU* c) {
#if defined(__x86_64__)
    static const u32 modes[] = {0, 0x4000, 0x2000, 0x6000};
    u32 value = (u32)audit_host_save() & ~(0x8040u | 0x6000u);
    value |= modes[(c->fpcr >> 22) & 3];
    if (c->fpcr & (1u << 24)) value |= 0x8040;
    audit_host_restore(value);
#else
    audit_host_restore(c->fpcr & 0x07C00000u);
#endif
}
