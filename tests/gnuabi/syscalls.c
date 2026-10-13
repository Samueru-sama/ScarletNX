/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright 2026 the ScarletNX contributors */
#include "guest.h"

static int same(const char* a, const char* b, unsigned n) {
    for (unsigned i = 0; i < n; ++i)
        if (a[i] != b[i]) return 0;
    return 1;
}
#define CHECK(test)                                                                                                    \
    do {                                                                                                               \
        if (!(test)) {                                                                                                 \
            emit_hex(__LINE__);                                                                                        \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)

int guest_main(u64* stack) {
    (void)stack;
    CHECK(call((1UL << 32) | 172, 0, 0, 0) > 0);
    char buffer[1024];
    const char* directory = "/tmp/scarletnx-gnuabi-test";
    const char* path = "/tmp/scarletnx-gnuabi-test/data";
    const char* text = "isolated guest filesystem\n";
    const int text_size = 26;
    call(35, -100, (u64)path, 0);
    call(35, -100, (u64)directory, 0x200);
    CHECK(call(34, -100, (u64)directory, 0700) == 0);
    s64 fd = syscall6(56, -100, (u64)path, 0x242, 0600, 0, 0); /* O_RDWR|O_CREAT|O_TRUNC */
    CHECK(fd >= 0);
    CHECK(call(64, fd, (u64)text, text_size) == text_size);
    CHECK(call(62, fd, 0, 0) == 0);
    CHECK(call(63, fd, (u64)buffer, sizeof(buffer)) == text_size);
    CHECK(same(buffer, text, text_size));
    u8 stat[128];
    CHECK(call(80, fd, (u64)stat, 0) == 0);
    /* asm-generic stat: st_size is a signed 64-bit field at offset 48. */
    CHECK(*(s64*)(stat + 48) == text_size);
    CHECK(call(57, fd, 0, 0) == 0);
    CHECK(syscall6(79, -100, (u64)path, (u64)stat, 0, 0, 0) == 0);
    fd = call(56, -100, (u64)directory, 0x4000); /* AArch64 O_DIRECTORY */
    CHECK(fd >= 0);
    CHECK(call(61, fd, (u64)buffer, sizeof(buffer)) > 0);
    CHECK(call(57, fd, 0, 0) == 0);
    CHECK(call(35, -100, (u64)path, 0) == 0);
    CHECK(call(35, -100, (u64)directory, 0x200) == 0);

    fd = call(56, -100, (u64) "/dev/zero", 0);
    CHECK(fd >= 0);
    CHECK(call(63, fd, (u64)buffer, sizeof(buffer)) == sizeof(buffer));
    for (unsigned i = 0; i < sizeof(buffer); ++i) CHECK(buffer[i] == 0);
    CHECK(call(57, fd, 0, 0) == 0);
    fd = call(56, -100, (u64) "/dev/null", 2);
    CHECK(fd >= 0);
    CHECK(call(64, fd, (u64)text, text_size) == text_size);
    CHECK(call(63, fd, (u64)buffer, sizeof(buffer)) == 0);
    CHECK(call(57, fd, 0, 0) == 0);
    fd = call(56, -100, (u64) "/dev/urandom", 0);
    CHECK(fd >= 0);
    CHECK(call(63, fd, (u64)buffer, 16) == 16);
    CHECK(call(57, fd, 0, 0) == 0);
    CHECK(syscall6(78, -100, (u64) "/proc/self/exe", (u64)buffer, sizeof(buffer), 0, 0) > 0);
    fd = call(56, -100, (u64) "/proc/self/maps", 0);
    CHECK(fd >= 0);
    CHECK(call(63, fd, (u64)buffer, sizeof(buffer)) > 0);
    CHECK(call(57, fd, 0, 0) == 0);
    CHECK(call(172, 0, 0, 0) > 0);            /* getpid */
    CHECK(call(178, 0, 0, 0) > 0);            /* gettid */
    CHECK(call(160, (u64)buffer, 0, 0) == 0); /* uname */
    CHECK(same(buffer + 65 * 4, "aarch64", 7));
    CHECK(call(113, 1, (u64)buffer, 0) == 0);   /* clock_gettime */
    CHECK(call(278, (u64)buffer, 16, 0) == 16); /* getrandom */
    CHECK(call(0x7fffffff, 0, 0, 0) == -38);    /* ENOSYS */
    emit_hex(0x73797363616c6c73UL);
    return 0;
}
