/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "process/process.h"
#include "abi/errno/errno.h"
#include "runtime/execution.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(expression)                                                                                              \
    do {                                                                                                               \
        if (!(expression)) {                                                                                           \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression);                                           \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)
static void put16(unsigned char* p, unsigned value) {
    p[0] = (unsigned char)value;
    p[1] = (unsigned char)(value >> 8);
}
static void put32(unsigned char* p, uint32_t value) {
    for (unsigned i = 0; i < 4; i++) p[i] = (unsigned char)(value >> (8 * i));
}
static void put64(unsigned char* p, uint64_t value) {
    for (unsigned i = 0; i < 8; i++) p[i] = (unsigned char)(value >> (8 * i));
}
static void executable(unsigned char* bytes, unsigned type) {
    memset(bytes, 0, 512);
    memcpy(bytes, "\177ELF\2\1\1", 7);
    put16(bytes + 16, type);
    put16(bytes + 18, 183);
    put32(bytes + 20, 1);
    put64(bytes + 24, type == 3 ? 0x100 : 0x400100);
    put64(bytes + 32, 64);
    put16(bytes + 52, 64);
    put16(bytes + 54, 56);
    put16(bytes + 56, 1);
    put32(bytes + 64, 1);
    put32(bytes + 68, 5);
    put64(bytes + 80, type == 3 ? 0 : 0x400000);
    put64(bytes + 96, 512);
    put64(bytes + 104, 8192);
    put64(bytes + 112, 4096);
    put32(bytes + 256, 0xd65f03c0);
}
static void faulting_guest(CPU* cpu, uint64_t entry) {
    (void)entry;
    cpu->x[0] = *(volatile unsigned char*)(cpu->mem + 0x80000);
}
int main(void) {
    GnuProcess* p = calloc(1, sizeof *p);
    CHECK(p);
    CHECK(gnu_memory_init(&p->memory) == 0);
    CHECK(gnu_memory_map(&p->memory, 0x10000, 8192, 3, 0) == 0);
    const char marker[] = "test";
    char copy[5];
    CHECK(gnu_memory_write(&p->memory, 0x10ffe, marker, sizeof marker) == 0);
    CHECK(gnu_memory_read(&p->memory, 0x10ffe, copy, sizeof copy) == 0 && !memcmp(copy, marker, sizeof copy));
    CHECK(gnu_memory_protect(&p->memory, 0x11000, 4096, 1) == 0);
    CHECK(gnu_memory_write(&p->memory, 0x10ffe, marker, sizeof marker) == -GNU_EFAULT);
    CHECK(gnu_memory_access(&p->memory, UINT64_MAX, 2, 1) == NULL);
    CHECK(gnu_memory_protect(&p->memory, 0x30000, 0, 1) == 0);
    CHECK(gnu_memory_protect(&p->memory, 0x30001, 0, 1) == -GNU_EINVAL);
    CHECK(gnu_memory_unmap(&p->memory, 0x11000, 4096) == 0);
    CHECK(gnu_memory_read(&p->memory, 0x11000, copy, 1) == -GNU_EFAULT);
    CHECK(gnu_memory_mmap(&p->memory, 0, 4096, 3, 0x22) >= 0x40000000);
    CHECK(gnu_memory_mmap(&p->memory, 0x10000, 4096, 3, 0x100022) == -GNU_EEXIST);
    CHECK(gnu_memory_mmap(&p->memory, 0x10001, 4096, 3, 0x32) == -GNU_EINVAL);
    CHECK(gnu_memory_unmap(&p->memory, 0x10000, 4096) == 0);
    gnu_memory_destroy(&p->memory);
    unsigned char bytes[512];
    executable(bytes, 2);
    char error[256] = {0};
    CHECK(gnu_memory_init(&p->memory) == 0);
    CHECK(gnu_elf_load_image(&p->elf, &p->memory, bytes, sizeof bytes, error, sizeof error) == 0);
    CHECK(p->elf.entry == 0x400100 && p->elf.phdr == 0x400040 && p->elf.code_count == 1);
    CHECK(gnu_memory_access(&p->memory, 0x400100, 4, 4) != NULL);
    CHECK(gnu_memory_write(&p->memory, 0x400100, marker, 1) == -GNU_EFAULT);
    unsigned char bss[16];
    memset(bss, 1, sizeof bss);
    CHECK(gnu_memory_read(&p->memory, 0x401000, bss, sizeof bss) == 0);
    for (size_t i = 0; i < sizeof bss; i++) CHECK(bss[i] == 0);
    const char* args[] = {"/program", "argument"};
    const char* env[] = {"KEY=value", NULL};
    CHECK(gnu_process_stack(p, 2, args, env) == 0);
    CHECK(!(p->cpu.x[31] & 15));
    uint64_t stack[7];
    CHECK(gnu_memory_read(&p->memory, p->cpu.x[31], stack, sizeof stack) == 0);
    CHECK(stack[0] == 2 && stack[3] == 0 && stack[5] == 0 && stack[6] == 3);
    CHECK(gnu_memory_read_string(&p->memory, stack[1], error, sizeof error) == 0 && !strcmp(error, "/program"));
    CHECK(gnu_memory_read_string(&p->memory, stack[4], error, sizeof error) == 0 && !strcmp(error, "KEY=value"));
    uint64_t initial = p->memory.brk_current;
    CHECK(gnu_memory_brk(&p->memory, initial + 5000) == initial + 5000);
    CHECK(gnu_memory_brk(&p->memory, UINT64_MAX) == initial + 5000);
    CHECK(gnu_memory_brk(&p->memory, initial) == initial);
    CHECK(gnu_vfs_init(&p->vfs) == 0);
    p->pid = p->tid = 1000;
    p->cpu.thread = p;
    uint64_t a[6] = {0};
    CHECK(gnu_syscall(p, UINT64_MAX, a) == -GNU_ENOSYS);
    CHECK(gnu_syscall(p, 172, a) == 1000);
    CHECK(gnu_syscall(p, UINT64_C(0x1000000ac), a) == 1000);
    a[0] = p->cpu.x[31] - 512;
    CHECK(gnu_syscall(p, 160, a) == 0);
    CHECK(gnu_memory_read(&p->memory, a[0] + 4 * 65, error, 65) == 0 && !strcmp(error, "aarch64"));
    a[0] = 0;
    a[1] = p->cpu.x[31] - 64;
    CHECK(gnu_syscall(p, 113, a) == 0);
    a[0] = p->cpu.x[31] - 128;
    a[1] = 16;
    a[2] = 0;
    CHECK(gnu_syscall(p, 278, a) == 16);
    uint32_t futex_value = 123;
    CHECK(gnu_memory_write(&p->memory, a[0], &futex_value, 4) == 0);
    a[0] = p->cpu.x[31] - 128;
    a[1] = 1;
    a[2] = 0;
    CHECK(gnu_syscall(p, 98, a) == 0);
    a[1] = 0;
    a[2] = 124;
    CHECK(gnu_syscall(p, 98, a) == -GNU_EAGAIN);
    a[0] = 1;
    a[1] = 0;
    a[2] = 0;
    a[3] = 7;
    CHECK(gnu_syscall(p, 134, a) == -GNU_EINVAL);
    a[0] = 42;
    gnu_syscall(p, 93, a);
    CHECK(p->exited && p->exit_code == 42);
    p->cpu.mem = p->memory.base;
    p->exited = 0;
    SnxExecution protected_execution = {0};
    protected_execution.entry = faulting_guest;
    CHECK(gnu_runtime_run(p, &protected_execution) == 139);
    CHECK(p->faulted == 11 && strstr(p->diagnostic, "mapped permissions"));
    gnu_process_destroy(p);
    memset(p, 0, sizeof *p);
    executable(bytes, 3);
    CHECK(gnu_memory_init(&p->memory) == 0);
    CHECK(gnu_elf_load_image(&p->elf, &p->memory, bytes, sizeof bytes, error, sizeof error) == 0);
    CHECK(p->elf.entry == 0x10000100 && p->elf.phdr == 0x10000040);
    gnu_elf_destroy(&p->elf);
    gnu_memory_destroy(&p->memory);
    executable(bytes, 2);
    put64(bytes + 104, UINT64_MAX);
    CHECK(gnu_memory_init(&p->memory) == 0);
    CHECK(gnu_elf_load_image(&p->elf, &p->memory, bytes, sizeof bytes, error, sizeof error) < 0);
    CHECK(strstr(error, "PT_LOAD") != NULL);
    executable(bytes, 2);
    put32(bytes + 64, 3);
    put64(bytes + 72, 256);
    put64(bytes + 96, 16);
    memcpy(bytes + 256, "/lib/ld.so", 11);
    CHECK(gnu_elf_load_image(&p->elf, &p->memory, bytes, sizeof bytes, error, sizeof error) < 0);
    CHECK(strstr(error, "interpreter") != NULL);
    gnu_memory_destroy(&p->memory);
    free(p);
    puts("GNU ABI memory, ELF, stack, syscall tests passed");
    return 0;
}
