/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "sha256.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static void check_hex(const unsigned char digest[32], const char* expected) {
    char hex[65];
    for (size_t i = 0; i < 32; ++i) snprintf(hex + i * 2, 3, "%02x", digest[i]);
    assert(!strcmp(hex, expected));
}
int main(void) {
    SnxSha256 context;
    unsigned char digest[32], whole[32], bytes[1024];
    snx_sha256(NULL, 0, digest);
    check_hex(digest, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    snx_sha256("abc", 3, digest);
    check_hex(digest, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    snx_sha256_init(&context);
    memset(bytes, 'a', 1000);
    for (unsigned i = 0; i < 1000; ++i) snx_sha256_update(&context, bytes, 1000);
    snx_sha256_final(&context, digest);
    check_hex(digest, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    for (size_t i = 0; i < sizeof bytes; ++i) bytes[i] = (unsigned char)(i * 37);
    for (size_t n = 0; n <= sizeof bytes; ++n) {
        snx_sha256(bytes, n, whole);
        for (size_t step = 1; step <= 130; step += 7) {
            snx_sha256_init(&context);
            snx_sha256_update(&context, NULL, 0);
            for (size_t at = 0; at < n;) {
                size_t count = n - at < step ? n - at : step;
                snx_sha256_update(&context, bytes + at, count);
                at += count;
            }
            snx_sha256_final(&context, digest);
            assert(!memcmp(digest, whole, 32));
        }
    }
    puts("SHA-256 published vectors and streaming boundary/chunk tests passed");
    return 0;
}
