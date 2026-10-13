/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "execution.h"
#include <stdint.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <signal.h>
#endif
static GnuProcess* active_process;
static void instruction_fetch(CPU* cpu, uint64_t address, uint32_t* word) {
    GnuProcess* p = cpu->thread;
    cpu->pc = address;
    void* instruction = gnu_memory_access(&p->memory, address, 4, GNU_PROT_EXEC);
    if ((address & 3) || !instruction) {
        *word = 0;
        gnu_process_fault(p, 11, "Guest instruction fetch violated mapped execution permissions");
        return;
    }
    /* Execute-only guest pages are host-readable instruction data. */
    memcpy(word, instruction, 4);
}
static int guest_address(uintptr_t address) {
    GnuProcess* p = active_process;
    uintptr_t base = p ? (uintptr_t)p->memory.base : 0;
    return p && p->running && address >= base && address - base < p->memory.size;
}
static void guest_fault(int signal) {
    GnuProcess* p = active_process;
    p->faulted = signal;
    p->exited = 1;
    p->exit_code = 128 + signal;
    static const char reason[] = "Guest memory access violated mapped permissions";
    /* Avoid allocation or stdio inside the host fault handler. */
    for (size_t i = 0; i < sizeof reason; i++) p->diagnostic[i] = reason[i];
    longjmp(p->exit_target, 1);
}
#ifdef _WIN32
static LONG CALLBACK access_fault(EXCEPTION_POINTERS* exception) {
    if (exception->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        exception->ExceptionRecord->NumberParameters >= 2 &&
        guest_address((uintptr_t)exception->ExceptionRecord->ExceptionInformation[1]))
        guest_fault(11);
    return EXCEPTION_CONTINUE_SEARCH;
}
#else
static struct sigaction previous_segv, previous_bus;
static void access_fault(int signal, siginfo_t* information, void* context) {
    (void)context;
    if (guest_address((uintptr_t)information->si_addr)) {
        sigset_t unblocked;
        sigemptyset(&unblocked);
        sigaddset(&unblocked, signal);
        (void)sigprocmask(SIG_UNBLOCK, &unblocked, NULL);
        guest_fault(signal);
    }
    (void)sigaction(signal, signal == SIGSEGV ? &previous_segv : &previous_bus, NULL);
    (void)raise(signal);
}
#endif
int gnu_runtime_run(GnuProcess* p, SnxExecution* execution) {
    active_process = p;
    execution->fetch = instruction_fetch;
#ifdef _WIN32
    PVOID guard = AddVectoredExceptionHandler(1, access_fault);
#else
    struct sigaction guard;
    memset(&guard, 0, sizeof guard);
    /* Specific fix for FreeBSD whom doesn't expose the sa_sigaction macro */
#if !defined(__linux__) && !defined(__XSI_VISIBLE)
    guard.__sigaction_u.__sa_sigaction = access_fault;
#else
    guard.sa_sigaction = access_fault;
#endif
    guard.sa_flags = SA_SIGINFO;
    sigemptyset(&guard.sa_mask);
    int have_segv = !sigaction(SIGSEGV, &guard, &previous_segv);
    int have_bus = !sigaction(SIGBUS, &guard, &previous_bus);
#endif
    if (!setjmp(p->exit_target)) {
        p->running = 1;
        snx_execution_run(execution, &p->cpu, p->elf.entry);
        if (!p->exited) gnu_process_fault(p, 4, "Guest entry returned without exit/exit_group");
    }
    p->running = 0;
#ifdef _WIN32
    if (guard) RemoveVectoredExceptionHandler(guard);
#else
    if (have_segv) (void)sigaction(SIGSEGV, &previous_segv, NULL);
    if (have_bus) (void)sigaction(SIGBUS, &previous_bus, NULL);
#endif
    active_process = NULL;
    return p->exit_code;
}
