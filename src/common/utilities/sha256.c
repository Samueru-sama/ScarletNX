/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "sha256.h"
#include <stdint.h>
#include <string.h>

static uint32_t ror(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }
static void block(uint32_t h[8], const unsigned char* p) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t w[64], a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], v = h[7];
    for (unsigned i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (unsigned i = 16; i < 64; i++) {
        uint32_t x = w[i - 15], y = w[i - 2];
        w[i] = w[i - 16] + (ror(x, 7) ^ ror(x, 18) ^ (x >> 3)) + w[i - 7] + (ror(y, 17) ^ ror(y, 19) ^ (y >> 10));
    }
    for (unsigned i = 0; i < 64; i++) {
        uint32_t t1 = v + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
        uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        v = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += v;
}
void snx_sha256_init(SnxSha256* context) {
    static const uint32_t initial[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(context->h, initial, sizeof initial);
    context->bytes = 0;
    context->used = 0;
}
void snx_sha256_update(SnxSha256* context, const void* data, size_t size) {
    const unsigned char* p = data;
    context->bytes += size;
    if (!size) return;
    if (context->used) {
        size_t n = 64 - context->used;
        if (n > size) n = size;
        memcpy(context->pending + context->used, p, n);
        context->used += n;
        p += n;
        size -= n;
        if (context->used == 64) {
            block(context->h, context->pending);
            context->used = 0;
        }
    }
    while (size >= 64) {
        block(context->h, p);
        p += 64;
        size -= 64;
    }
    if (size) {
        memcpy(context->pending, p, size);
        context->used = size;
    }
}
void snx_sha256_final(SnxSha256* context, unsigned char digest[32]) {
    unsigned char tail[128] = {0};
    uint64_t bits = context->bytes * 8;
    size_t size = context->used, end = size < 56 ? 64 : 128;
    if (size) memcpy(tail, context->pending, size);
    tail[size] = 0x80;
    for (unsigned i = 0; i < 8; i++) tail[end - 1 - i] = (unsigned char)(bits >> (8 * i));
    block(context->h, tail);
    if (end == 128) block(context->h, tail + 64);
    for (unsigned i = 0; i < 32; i++) digest[i] = (unsigned char)(context->h[i / 4] >> (24 - 8 * (i % 4)));
    memset(context, 0, sizeof *context);
}
void snx_sha256(const void* data, size_t size, unsigned char digest[32]) {
    SnxSha256 context;
    snx_sha256_init(&context);
    snx_sha256_update(&context, data, size);
    snx_sha256_final(&context, digest);
}
