/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "signals.h"
#include "../process/process.h"
#include "../abi/errno/errno.h"
int64_t gnu_sigaction(GnuProcess* p, const uint64_t a[6]) {
    if (a[0] < 1 || a[0] > 64 || (a[1] && (a[0] == 9 || a[0] == 19)) || a[3] != 8) return -GNU_EINVAL;
    GnuSignalAction action;
    if (a[1] && gnu_memory_read(&p->memory, a[1], &action, sizeof action)) return -GNU_EFAULT;
    if (a[2] && gnu_memory_write(&p->memory, a[2], &p->actions[a[0]], sizeof action)) return -GNU_EFAULT;
    if (a[1]) {
        action.mask &= ~((UINT64_C(1) << 8) | (UINT64_C(1) << 18));
        p->actions[a[0]] = action;
    }
    return 0;
}
int64_t gnu_sigprocmask(GnuProcess* p, const uint64_t a[6]) {
    if (a[3] != 8) return -GNU_EINVAL;
    uint64_t mask = 0;
    if (a[1] && (a[0] > 2 || gnu_memory_read(&p->memory, a[1], &mask, 8))) return a[0] > 2 ? -GNU_EINVAL : -GNU_EFAULT;
    if (a[2] && gnu_memory_write(&p->memory, a[2], &p->signal_mask, 8)) return -GNU_EFAULT;
    mask &= ~((UINT64_C(1) << 8) | (UINT64_C(1) << 18));
    if (a[1]) {
        if (a[0] == 0)
            p->signal_mask |= mask;
        else if (a[0] == 1)
            p->signal_mask &= ~mask;
        else
            p->signal_mask = mask;
    }
    return 0;
}
