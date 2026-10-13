/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright 2026 the ScarletNX contributors */
/* Removing execute permission must fault at the instruction after SVC. */
.text
.global _start
.type _start, %function
_start:
    adr x0, _start
    bic x0, x0, #4095
    mov x1, #4096
    mov x2, #1
    mov x8, #226
    svc #0
    mov x0, #17
    mov x8, #93
    svc #0
.size _start, .-_start
.section .note.GNU-stack,"",%progbits
