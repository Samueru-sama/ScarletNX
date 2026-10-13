/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright 2026 the ScarletNX contributors */
.text
.balign 4096
.space 4092
.global boundary_probe
.type boundary_probe, %function
boundary_probe:
    adds x0, x0, #1
    b.cs 1f
    eor x0, x0, #0xff
1:
    cbz x0, 2f
    ror x0, x0, #7
2:
    ret
.size boundary_probe, .-boundary_probe
.section .note.GNU-stack,"",%progbits
