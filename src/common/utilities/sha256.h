/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_SHA256_H
#define SNX_SHA256_H
#include <stddef.h>
#include <stdint.h>
typedef struct {
    uint32_t h[8];
    uint64_t bytes;
    size_t used;
    unsigned char pending[64];
} SnxSha256;
void snx_sha256_init(SnxSha256* context);
void snx_sha256_update(SnxSha256* context, const void* data, size_t size);
/* Finalization consumes the context. Lengths are modulo 2^64 bits as in SHA-256. */
void snx_sha256_final(SnxSha256* context, unsigned char digest[32]);
/* SHA-256 of a byte string (FIPS 180-4), in network byte order. */
void snx_sha256(const void* data, size_t size, unsigned char digest[32]);
#endif
