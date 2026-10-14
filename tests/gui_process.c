/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#define _POSIX_C_SOURCE 200809L
#include "process.h"
#include "common.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#include <unistd.h>
#endif
static double now(void) {
#ifdef _WIN32
    return GetTickCount64() / 1000.0;
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
#endif
}
static void delay(unsigned ms) {
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000};
    nanosleep(&t, NULL);
#endif
}
static void grandchild(const char* self, const char* mode, const char* marker) {
#ifdef _WIN32
    char* command = snx_format("\"%s\" %s \"%s\"", self, mode, marker);
    int n = MultiByteToWideChar(CP_UTF8, 0, command, -1, NULL, 0);
    wchar_t* wide = calloc((size_t)n, sizeof(*wide));
    assert(wide && MultiByteToWideChar(CP_UTF8, 0, command, -1, wide, n));
    STARTUPINFOW startup = {0};
    PROCESS_INFORMATION child = {0};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    assert(CreateProcessW(NULL, wide, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &startup, &child));
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    free(wide);
    free(command);
#else
    pid_t pid = fork();
    assert(pid >= 0);
    if (!pid) {
        /* The real ScarletNX driver makes separate stage process groups too. */
        assert(!setpgid(0, 0));
        execl(self, self, mode, marker, (char*)NULL);
        _exit(127);
    }
#endif
}
int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    if (argc > 1 && !strcmp(argv[1], "--child")) {
        assert(argc == 4 && !strcmp(argv[2], "a b;$(literal)\"\\") && !strcmp(argv[3], ""));
        puts("child stdout");
        fputs("child stderr\n", stderr);
        grandchild(argv[0], "--grandchild", "");
        return 7; /* Grandchild outlives the immediate child. */
    }
    if (argc > 1 && !strcmp(argv[1], "--grandchild")) {
        unsigned char* memory = malloc(32 * 1024 * 1024);
        assert(memory);
        memset(memory, 42, 32 * 1024 * 1024);
        char block[4096];
        memset(block, 'x', sizeof(block));
        for (int i = 0; i < 100; ++i) assert(fwrite(block, 1, sizeof(block), stdout) == sizeof(block));
        double end = now() + 0.8;
        volatile unsigned n = 0;
        while (now() < end) ++n;
        printf("\ngrandchild stdout %u %u\n", memory[n % (32 * 1024 * 1024)], n);
        fputs("grandchild stderr\n", stderr);
        free(memory);
        return 0;
    }
    if (argc > 1 && (!strcmp(argv[1], "--linger") || !strcmp(argv[1], "--orphan"))) {
#ifndef _WIN32
        signal(SIGTERM, SIG_IGN);
#endif
        grandchild(argv[0], "--sleeper", argv[2]);
        if (!strcmp(argv[1], "--orphan")) return 0;
        for (;;) delay(1000);
    }
    if (argc > 1 && !strcmp(argv[1], "--sleeper")) {
#ifndef _WIN32
        signal(SIGTERM, SIG_IGN);
#endif
        puts("ready");
        delay(1000);
        assert(!snx_write_file(argv[2], "escaped", 7));
        return 0;
    }
    GuiProcess* process = gui_process_create();
    assert(process);
    char* self = snx_executable(argv[0]);
    char error[512];
    const char* missing[] = {"/nonexistent/scarletnx-gui-driver", NULL};
    assert(!gui_process_start(process, missing, error, sizeof(error)));
    GuiUsage baseline = gui_process_usage(process);
    assert(baseline.available && baseline.memory_bytes > 0);
    const char* command[] = {self, "--child", "a b;$(literal)\"\\", "", NULL};
    assert(gui_process_start(process, command, error, sizeof(error)));
    unsigned found = 0;
    size_t bytes = 0;
    bool saw_cpu = false, saw_memory = false;
    double end = now() + 10;
    char buffer[8192] = {0};
    size_t carry = 0;
    int status = -1;
    while (now() < end) {
        int n = gui_process_read(process, buffer + carry, 4096);
        if (n > 0) {
            size_t length = carry + (size_t)n;
            buffer[length] = 0;
            if (strstr(buffer, "child stdout")) found |= 1;
            if (strstr(buffer, "child stderr")) found |= 2;
            if (strstr(buffer, "grandchild stdout")) found |= 4;
            if (strstr(buffer, "grandchild stderr")) found |= 8;
            bytes += (size_t)n;
            carry = length < 64 ? length : 64;
            memmove(buffer, buffer + length - carry, carry);
        }
        GuiUsage usage = gui_process_usage(process);
        saw_cpu |= usage.cpu_percent > 0;
        saw_memory |= usage.memory_bytes > baseline.memory_bytes + 16 * 1024 * 1024;
        if (gui_process_finished(process, &status)) break;
        delay(2);
    }
    assert(status == 7 && found == 15 && bytes > 409600 && saw_cpu && saw_memory);
    char* temporary = snx_temp_dir(NULL);
    char* marker = snx_path_join(temporary, "must not exist");
    /* Force termination covers a hung parent, an exited parent with surviving
       descendants, and the existing close-window cleanup path. Reuse must work. */
    for (int pass = 0; pass < 3; ++pass) {
        const char* linger[] = {self, pass == 1 ? "--orphan" : "--linger", marker, NULL};
        assert(gui_process_start(process, linger, error, sizeof(error)));
        end = now() + 5;
        bool ready = false;
        while (now() < end) {
            int n = gui_process_read(process, buffer, sizeof(buffer) - 1);
            if (n > 0) {
                buffer[n] = 0;
                if (strstr(buffer, "ready")) {
                    ready = true;
                    break;
                }
            }
            delay(10);
        }
        assert(ready && !gui_process_finished(process, &status));
        if (pass == 2) {
            gui_process_destroy(process);
        } else {
            double started = now();
            gui_process_terminate(process);
            assert(now() - started < 1);
            gui_process_terminate(process); /* Idempotent while awaiting exit. */
            bool finished = false;
            while (now() - started < 2) {
                gui_process_read(process, buffer, sizeof(buffer));
                if ((finished = gui_process_finished(process, &status))) break;
                delay(5);
            }
            assert(finished);
        }
        delay(1200);
        assert(!snx_is_file(marker));
    }
    snx_remove_tree(temporary);
    free(marker);
    free(temporary);
    free(self);
    puts("GUI process tree: inherited output, quoting, usage, exit and cleanup passed");
    return 0;
}
