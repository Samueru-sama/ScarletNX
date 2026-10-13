/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright 2026 the ScarletNX contributors */
/* Changed code must not run the old, statically emitted instruction. */
.text
.global _start
.type _start, %function
_start:
    adr x0, target
    bic x0, x0, #4095
    mov x1, #4096
    mov x2, #7
    mov x8, #226
    svc #0
    adr x1, target
    mov w2, #0x0540
    movk w2, #0xd280, lsl #16
    str w2, [x1]
    dc cvau, x1
    dsb ish
    ic ivau, x1
    dsb ish
    isb
    bl target
    mov x8, #93
    svc #0
target:
    mov x0, #17
    ret
.size _start, .-_start
.section .note.GNU-stack,"",%progbits
