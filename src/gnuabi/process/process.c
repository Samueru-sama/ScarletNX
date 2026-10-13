/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "process.h"
#include "../abi/errno/errno.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#endif

int gnu_random_bytes(void* destination, size_t size) {
#ifdef _WIN32
    if (size > UINT32_MAX) return -GNU_EINVAL;
    return BCryptGenRandom(NULL, destination, (ULONG)size, BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0 ? -GNU_EIO : 0;
#else
    FILE* random = fopen("/dev/urandom", "rb");
    if (!random) return -GNU_EIO;
    size_t n = fread(destination, 1, size, random);
    fclose(random);
    return n == size ? 0 : -GNU_EIO;
#endif
}
int gnu_clock_gettime(int id, int64_t* seconds, int64_t* nanos) {
    if (id != 0 && id != 1 && id != 4 && id != 5 && id != 6 && id != 7 && id != 2 && id != 3) return -GNU_EINVAL;
#ifdef _WIN32
    if (id == 0 || id == 5) {
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        uint64_t ticks = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
        ticks -= UINT64_C(116444736000000000);
        *seconds = (int64_t)(ticks / 10000000);
        *nanos = (int64_t)(ticks % 10000000) * 100;
    } else if (id == 2 || id == 3) {
        FILETIME created, exit_time, kernel, user;
        BOOL ok = id == 2 ? GetProcessTimes(GetCurrentProcess(), &created, &exit_time, &kernel, &user)
                          : GetThreadTimes(GetCurrentThread(), &created, &exit_time, &kernel, &user);
        if (!ok) return -GNU_EIO;
        uint64_t ticks = (((uint64_t)kernel.dwHighDateTime << 32) | kernel.dwLowDateTime) +
                         (((uint64_t)user.dwHighDateTime << 32) | user.dwLowDateTime);
        *seconds = (int64_t)(ticks / 10000000);
        *nanos = (int64_t)(ticks % 10000000) * 100;
    } else {
        LARGE_INTEGER counter, frequency;
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&counter);
        *seconds = counter.QuadPart / frequency.QuadPart;
        *nanos = (int64_t)((counter.QuadPart % frequency.QuadPart) * 1000000000 / frequency.QuadPart);
    }
#else
    clockid_t host = id == 0 || id == 5 ? CLOCK_REALTIME
                     : id == 2          ? CLOCK_PROCESS_CPUTIME_ID
                     : id == 3          ? CLOCK_THREAD_CPUTIME_ID
                                        : CLOCK_MONOTONIC;
    struct timespec time;
    if (clock_gettime(host, &time)) return -GNU_EIO;
    *seconds = time.tv_sec;
    *nanos = time.tv_nsec;
#endif
    return 0;
}
static int push(GnuProcess* p, uint64_t* sp, const void* data, size_t n) {
    if (n > *sp || *sp - n < UINT64_C(0xef800000)) return -GNU_E2BIG;
    *sp -= n;
    return gnu_memory_write(&p->memory, *sp, data, n);
}
int gnu_process_stack(GnuProcess* p, int argc, const char* const* argv, const char* const* env) {
    uint64_t sp = UINT64_C(0xf0000000), stack_low = UINT64_C(0xef800000);
    if (argc < 1 || argc > 65536) return -GNU_EINVAL;
    size_t envc = 0;
    if (env)
        while (env[envc]) {
            if (++envc > 65536) return -GNU_E2BIG;
        }
    if (gnu_memory_map(&p->memory, stack_low, sp - stack_low, 3, 0)) return -GNU_ENOMEM;
    uint64_t* arguments = calloc((size_t)argc, sizeof *arguments);
    uint64_t* environment = calloc(envc ? envc : 1, sizeof *environment);
    if (!arguments || !environment) {
        free(arguments);
        free(environment);
        return -GNU_ENOMEM;
    }
    int result = 0;
    for (size_t i = envc; i > 0; i--) {
        if ((result = push(p, &sp, env[i - 1], strlen(env[i - 1]) + 1))) goto done;
        environment[i - 1] = sp;
    }
    for (int i = argc; i > 0; i--) {
        if (!argv[i - 1] || (result = push(p, &sp, argv[i - 1], strlen(argv[i - 1]) + 1))) {
            result = -GNU_E2BIG;
            goto done;
        }
        arguments[i - 1] = sp;
    }
    const char platform[] = "aarch64";
    if ((result = push(p, &sp, platform, sizeof platform))) goto done;
    uint64_t platform_address = sp;
    unsigned char random[16];
    if ((result = gnu_random_bytes(random, sizeof random)) || (result = push(p, &sp, random, sizeof random))) goto done;
    uint64_t random_address = sp;
    /* Features advertised here must be implemented by the shared recompiler. */
    const uint64_t auxiliary[] = {3,  p->elf.phdr,
                                  4,  p->elf.phentsize,
                                  5,  p->elf.phnum,
                                  6,  GNU_PAGE_SIZE,
                                  7,  0,
                                  8,  0,
                                  9,  p->elf.entry,
                                  11, 1000,
                                  12, 1000,
                                  13, 1000,
                                  14, 1000,
                                  15, platform_address,
                                  16, 3,
                                  17, 100,
                                  23, 0,
                                  25, random_address,
                                  26, 0,
                                  31, arguments[0],
                                  0,  0};
    size_t words = 1 + (size_t)argc + 1 + envc + 1 + sizeof auxiliary / sizeof auxiliary[0];
    uint64_t bytes = (uint64_t)words * 8;
    uint64_t initial = (sp - bytes) & ~UINT64_C(15);
    if (initial < stack_low) {
        result = -GNU_E2BIG;
        goto done;
    }
    uint64_t* values = calloc(words, 8);
    if (!values) {
        result = -GNU_ENOMEM;
        goto done;
    }
    size_t index = 0;
    values[index++] = (uint64_t)argc;
    for (int i = 0; i < argc; i++) values[index++] = arguments[i];
    values[index++] = 0;
    for (size_t i = 0; i < envc; i++) values[index++] = environment[i];
    values[index++] = 0;
    memcpy(values + index, auxiliary, sizeof auxiliary);
    result = gnu_memory_write(&p->memory, initial, values, (size_t)bytes);
    free(values);
    if (!result) {
        p->cpu.x[31] = initial;
        p->cpu.pc = p->elf.entry;
        /* Linux starts all general registers at zero; argc/argv live on the stack. */
        result = gnu_memory_protect(&p->memory, stack_low, UINT64_C(0x800000), p->elf.stack_permissions);
    }
done:
    free(arguments);
    free(environment);
    return result;
}
int gnu_process_init(GnuProcess* p, const char* path, int argc, const char* const* argv, const char* const* env,
                     char* error, size_t cap) {
    if (cap) error[0] = 0;
    memset(p, 0, sizeof *p);
    p->pid = p->tid = 1000;
    int r = gnu_memory_init(&p->memory);
    if (!r) r = gnu_vfs_init(&p->vfs);
    if (!r) r = gnu_elf_load(&p->elf, &p->memory, path, error, cap);
    if (!r) {
        p->cpu.mem = p->memory.base;
        p->cpu.rt = &gnu_rtapi;
        p->cpu.thread = p;
        r = gnu_process_stack(p, argc, argv, env);
    }
    if (!r) r = gnu_vfs_set_process(&p->vfs, p->pid, "/program", argc, argv);
    if (!r) r = gnu_vfs_set_executable(&p->vfs, p->elf.image, p->elf.size);
    if (r) {
        if (cap && !error[0]) snprintf(error, cap, "Guest process initialization failed (%d)", r);
        gnu_process_destroy(p);
    }
    return r;
}
void gnu_process_destroy(GnuProcess* p) {
    gnu_vfs_destroy(&p->vfs);
    gnu_elf_destroy(&p->elf);
    gnu_memory_destroy(&p->memory);
}
void gnu_process_exit(GnuProcess* p, int status) {
    if (p->clear_tid) {
        uint32_t zero = 0;
        (void)gnu_memory_write(&p->memory, p->clear_tid, &zero, 4);
    }
    p->exited = 1;
    p->exit_code = status & 255;
    if (p->running) longjmp(p->exit_target, 1);
}
void gnu_process_fault(GnuProcess* p, int signal, const char* reason) {
    p->faulted = signal;
    snprintf(p->diagnostic, sizeof p->diagnostic, "%s", reason);
    gnu_process_exit(p, 128 + signal);
}
