/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_SHA256_H
#define SNX_SHA256_H
#include <stddef.h>
/* SHA-256 of a byte string (FIPS 180-4), in network byte order. */
void snx_sha256(const void* data, size_t size, unsigned char digest[32]);
#endif
