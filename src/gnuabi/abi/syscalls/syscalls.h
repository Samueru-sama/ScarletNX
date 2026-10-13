/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_GNU_SYSCALLS_H
#define SNX_GNU_SYSCALLS_H
/* AArch64 asm-generic numbers; arguments x0-x5, number x8, result x0. */
enum {
    GNU_NR_exit = 93,
    GNU_NR_exit_group = 94,
    GNU_NR_set_tid_address = 96,
    GNU_NR_futex = 98,
    GNU_NR_clock_gettime = 113,
    GNU_NR_rt_sigaction = 134,
    GNU_NR_rt_sigprocmask = 135,
    GNU_NR_uname = 160,
    GNU_NR_getpid = 172,
    GNU_NR_gettid = 178,
    GNU_NR_brk = 214,
    GNU_NR_munmap = 215,
    GNU_NR_mmap = 222,
    GNU_NR_mprotect = 226,
    GNU_NR_getrandom = 278
};
#endif
