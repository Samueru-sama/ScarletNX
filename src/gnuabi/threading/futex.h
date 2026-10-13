/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_GNU_FUTEX_H
#define SNX_GNU_FUTEX_H
#include <stdint.h>
struct GnuProcess;
int64_t gnu_futex(struct GnuProcess* process, const uint64_t arguments[6]);
#endif
