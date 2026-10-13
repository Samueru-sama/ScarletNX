/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "futex.h"
#include "../process/process.h"
#include "../abi/errno/errno.h"
#include <time.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
int64_t gnu_futex(GnuProcess* p, const uint64_t a[6]) {
    uint32_t current;
    if (a[0] & 3) return -GNU_EINVAL;
    if (gnu_memory_read(&p->memory, a[0], &current, 4)) return -GNU_EFAULT;
    unsigned operation = (unsigned)a[1] & 127u;
    if (operation == 1 || operation == 10) {
        if ((int32_t)a[2] < 0) return -GNU_EINVAL;
        if (operation == 10 && !a[5]) return -GNU_EINVAL;
        return 0; /* A single guest thread has no waiting peers. */
    }
    if (operation != 0 && operation != 9) return -GNU_ENOSYS;
    if (operation == 9 && !a[5]) return -GNU_EINVAL;
    if (current != (uint32_t)a[2]) return -GNU_EAGAIN;
    if (!a[3]) return -GNU_ENOSYS; /* Blocking without a peer cannot make progress. */
    int64_t timeout[2];
    if (gnu_memory_read(&p->memory, a[3], timeout, sizeof timeout)) return -GNU_EFAULT;
    if (timeout[0] < 0 || timeout[1] < 0 || timeout[1] >= 1000000000) return -GNU_EINVAL;
    int64_t seconds = timeout[0], nanos = timeout[1];
    if (operation == 9) {
        int64_t now_seconds, now_nanos;
        if (gnu_clock_gettime(a[1] & 256 ? 0 : 1, &now_seconds, &now_nanos)) return -GNU_EINVAL;
        seconds -= now_seconds;
        nanos -= now_nanos;
        if (nanos < 0) {
            seconds--;
            nanos += 1000000000;
        }
        if (seconds < 0) return -GNU_ETIMEDOUT;
    } else if (a[1] & 256)
        return -GNU_ENOSYS;
#ifdef _WIN32
    /* Sleep in bounded chunks without overflowing the Windows timeout. */
    while (seconds > 3600) {
        Sleep(3600000);
        seconds -= 3600;
    }
    Sleep((DWORD)(seconds * 1000 + (nanos + 999999) / 1000000));
#else
    struct timespec duration = {(time_t)seconds, (long)nanos};
    while (nanosleep(&duration, &duration)) {}
#endif
    return -GNU_ETIMEDOUT;
}
