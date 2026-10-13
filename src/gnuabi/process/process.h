/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_GNU_PROCESS_H
#define SNX_GNU_PROCESS_H
#include "cpu/aarch64/cpu.h"
#include "../memory/memory.h"
#include "../loader/elf/elf.h"
#include "../filesystem/vfs.h"
#include "../abi/structures/signal.h"
#include <setjmp.h>
typedef struct GnuProcess {
    CPU cpu;
    GnuMemory memory;
    GnuElf elf;
    GnuVfs vfs;
    uint32_t pid, tid;
    uint64_t clear_tid, signal_mask;
    GnuSignalAction actions[65];
    jmp_buf exit_target;
    int running, exited, exit_code, faulted;
    char diagnostic[256];
} GnuProcess;
int gnu_random_bytes(void* destination, size_t size);
int gnu_clock_gettime(int clock_id, int64_t* seconds, int64_t* nanoseconds);
int gnu_process_init(GnuProcess* process, const char* path, int argc, const char* const* argv,
                     const char* const* environment, char* error, size_t error_capacity);
int gnu_process_stack(GnuProcess* process, int argc, const char* const* argv, const char* const* environment);
void gnu_process_destroy(GnuProcess* process);
void gnu_process_exit(GnuProcess* process, int status);
void gnu_process_fault(GnuProcess* process, int signal, const char* reason);
int64_t gnu_syscall(GnuProcess* process, uint64_t number, const uint64_t arguments[6]);
extern const RtAPI gnu_rtapi;
#endif
