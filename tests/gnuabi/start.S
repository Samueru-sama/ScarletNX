/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright 2026 the ScarletNX contributors */
.text
.global _start
.type _start, %function
_start:
    mov x0, sp
    bl guest_main
    mov x8, #93
    svc #0
    brk #0
.size _start, .-_start
.section .note.GNU-stack,"",%progbits
