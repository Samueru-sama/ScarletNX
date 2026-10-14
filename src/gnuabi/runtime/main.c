/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "../process/process.h"
#include "cpu/execution/execution.h"
#include "execution.h"
#include "arm_exec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    const char* cc = NULL;
    int first = 1;
    while (first < argc && !strncmp(argv[first], "--", 2)) {
        if (!strcmp(argv[first], "--help")) {
            puts(
                "Usage: scarletnx-gnuabi [--cc=COMPILER] [--] ELF [arguments...]\n"
                "Executes static Linux AArch64 or AArch32 (A32/Thumb) ELF files through the ScarletNX recompiler.\n"
                "The guest filesystem is isolated; /tmp is ephemeral. Dynamic interpreters and clone are unsupported.");
            return 0;
        }
        if (!strncmp(argv[first], "--cc=", 5))
            cc = argv[first] + 5;
        else if (!strcmp(argv[first], "--")) {
            first++;
            break;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[first]);
            return 2;
        }
        first++;
    }
    if (first == argc) {
        fprintf(stderr, "Usage: scarletnx-gnuabi [--cc=COMPILER] ELF [arguments...]\n");
        return 2;
    }
    GnuProcess* process = calloc(1, sizeof *process);
    if (!process) return 2;
    char error[1024] = {0};
    const char* const environment[] = {"PATH=/bin", "LANG=C", NULL};
    if (gnu_process_init(process, argv[first], argc - first, (const char* const*)(argv + first), environment, error,
                         sizeof error)) {
        fprintf(stderr, "scarletnx-gnuabi: %s\n", error);
        free(process);
        return 2;
    }
    SnxExecRange* ranges = calloc(process->elf.code_count, sizeof *ranges);
    if (!ranges) {
        gnu_process_destroy(process);
        free(process);
        return 2;
    }
    if (process->elf.bits == 32) {
        /* AArch32 runs the ARM LLVM block backend directly, without the A64
           recompiling step. */
        free(ranges);
        int status = gnu_arm_run(process);
        if (process->faulted) fprintf(stderr, "scarletnx-gnuabi: %s\n", process->diagnostic);
        gnu_process_destroy(process);
        free(process);
        return status;
    }
    for (size_t i = 0; i < process->elf.code_count; i++) {
        ranges[i].address = process->elf.code[i].address;
        ranges[i].data = process->elf.code[i].data;
        ranges[i].size = process->elf.code[i].size;
    }
    SnxExecution execution = {0};
    if (snx_execution_compile(&execution, ranges, process->elf.code_count, process->elf.entry, cc, error,
                              sizeof error)) {
        fprintf(stderr, "scarletnx-gnuabi: %s\n", error);
        free(ranges);
        gnu_process_destroy(process);
        free(process);
        return 2;
    }
    free(ranges);
    int status = gnu_runtime_run(process, &execution);
    if (process->faulted) fprintf(stderr, "scarletnx-gnuabi: %s\n", process->diagnostic);
    snx_execution_free(&execution);
    gnu_process_destroy(process);
    free(process);
    return status;
}
