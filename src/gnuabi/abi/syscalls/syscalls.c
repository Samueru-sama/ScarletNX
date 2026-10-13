/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "syscalls.h"
#include "../errno/errno.h"
#include "../../process/process.h"
#include "../../signals/signals.h"
#include "../../threading/futex.h"
#include <string.h>
#include <stdio.h>
static int64_t file_mmap(GnuProcess* p, const uint64_t a[6]) {
    if (a[2] & ~UINT64_C(7)) return -GNU_EINVAL;
    if (a[5] & 4095) return -GNU_EINVAL;
    if ((a[3] & 3) != 2) return -GNU_ENOTSUP; /* Shared file writeback needs a coherent VFS mapping. */
    unsigned char probe;
    int64_t r = gnu_vfs_pread(&p->vfs, (int)a[4], &probe, 0, a[5]);
    if (r < 0) return r;
    int64_t mapped = gnu_memory_mmap(&p->memory, a[0], a[1], 3, a[3] | 0x20);
    if (mapped < 0) return mapped;
    r = gnu_vfs_pread(&p->vfs, (int)a[4], p->memory.base + (uint64_t)mapped, (size_t)a[1], a[5]);
    if (r >= 0) r = gnu_memory_protect(&p->memory, (uint64_t)mapped, a[1], (unsigned)a[2]);
    if (r < 0) {
        gnu_memory_unmap(&p->memory, (uint64_t)mapped, a[1]);
        return r;
    }
    return mapped;
}
int64_t gnu_syscall(GnuProcess* p, uint64_t number, const uint64_t a[6]) {
    int64_t result;
    /* arm64 el0_svc_common takes an int syscall number from x8. */
    number = (uint32_t)number;
    if (gnu_vfs_syscall(&p->vfs, &p->memory, number, a, &result)) return result;
    switch (number) {
        case GNU_NR_exit:
        case GNU_NR_exit_group: gnu_process_exit(p, (int)a[0]); return 0;
        case GNU_NR_getpid: return p->pid;
        case GNU_NR_gettid: return p->tid;
        case 173: return 1; /* getppid */
        case 174:
        case 175:
        case 176:
        case 177: return 1000; /* uid/euid/gid/egid */
        case GNU_NR_set_tid_address: p->clear_tid = a[0]; return p->tid;
        case GNU_NR_futex: return gnu_futex(p, a);
        case GNU_NR_rt_sigaction: return gnu_sigaction(p, a);
        case GNU_NR_rt_sigprocmask: return gnu_sigprocmask(p, a);
        case GNU_NR_brk: return (int64_t)gnu_memory_brk(&p->memory, a[0]);
        case GNU_NR_munmap: return gnu_memory_unmap(&p->memory, a[0], a[1]);
        case GNU_NR_mprotect:
            if (a[2] & ~UINT64_C(7)) return -GNU_EINVAL;
            return gnu_memory_protect(&p->memory, a[0], a[1], (unsigned)a[2]);
        case GNU_NR_mmap:
            if (a[2] & ~UINT64_C(7)) return -GNU_EINVAL;
            if (!(a[3] & 0x20)) return file_mmap(p, a);
            if (a[5] & 4095) return -GNU_EINVAL;
            return gnu_memory_mmap(&p->memory, a[0], a[1], (unsigned)a[2], a[3]);
        case GNU_NR_uname: {
            char uts[6][65];
            memset(uts, 0, sizeof uts);
            strcpy(uts[0], "Linux");
            strcpy(uts[1], "scarletnx");
            strcpy(uts[2], "6.6.0");
            strcpy(uts[3], "ScarletNX GNU ABI");
            strcpy(uts[4], "aarch64");
            strcpy(uts[5], "(none)");
            return gnu_memory_write(&p->memory, a[0], uts, sizeof uts);
        }
        case GNU_NR_clock_gettime: {
            int64_t ts[2];
            result = gnu_clock_gettime((int)a[0], &ts[0], &ts[1]);
            return result ? result : gnu_memory_write(&p->memory, a[1], ts, sizeof ts);
        }
        case 114: { /* clock_getres */
            int64_t sec, ns;
            result = gnu_clock_gettime((int)a[0], &sec, &ns);
            if (result || !a[1]) return result;
            int64_t resolution[2] = {0, 100};
            return gnu_memory_write(&p->memory, a[1], resolution, sizeof resolution);
        }
        case GNU_NR_getrandom: {
            if ((a[2] & ~UINT64_C(7)) || ((a[2] & 6) == 6)) return -GNU_EINVAL;
            size_t count = a[1] > UINT64_C(0x2000000) ? 0x2000000 : (size_t)a[1];
            void* destination = gnu_memory_access(&p->memory, a[0], count, GNU_PROT_WRITE);
            if (!destination) return -GNU_EFAULT;
            result = gnu_random_bytes(destination, count);
            return result ? result : (int64_t)count;
        }
        case 261: { /* prlimit64: query guest resource limits */
            if (a[0] && a[0] != p->pid) return -GNU_ESRCH;
            if (a[1] >= 16) return -GNU_EINVAL;
            if (a[2]) return -GNU_ENOSYS;
            uint64_t limits[2] = {UINT64_MAX, UINT64_MAX};
            if (a[1] == 3) limits[0] = UINT64_C(0x800000);
            if (a[1] == 7) limits[0] = limits[1] = 256;
            return a[3] ? gnu_memory_write(&p->memory, a[3], limits, sizeof limits) : 0;
        }
        case 167: {                   /* prctl */
            if (a[0] == 3) return 1;  /* PR_GET_DUMPABLE */
            if (a[0] == 21) return 0; /* PR_GET_SECCOMP */
            return -GNU_EINVAL;
        }
        case 124: return 0; /* sched_yield; one guest thread */
        default: return -GNU_ENOSYS;
    }
}
