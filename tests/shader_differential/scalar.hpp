// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: Copyright 2026 ScarletNX contributors
// CPU compatibility layer for a bounded scalar GLSL subset. NOT a GPU backend.
#pragma once
#include <algorithm>
#include <bit>
#include <cfenv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
using uint = uint32_t;
using std::min;
using std::max;
using std::clamp;
using std::isnan;
using std::abs;
using std::floor;
using std::ceil;
using std::trunc;
static_assert(sizeof(int) == 4 && sizeof(float) == 4);
inline float uintBitsToFloat(uint x) { return std::bit_cast<float>(x); }
inline uint floatBitsToUint(float x) { return std::bit_cast<uint>(x); }
inline float roundEven(float x) { return std::nearbyint(x); }
struct vec2 {
    float x, y;
    explicit vec2(float a) : x(a), y(a) {}
    vec2(float a, float b) : x(a), y(b) {}
};
struct CBuffer {
    struct {
        float x, y, z, w;
    } d[1];
    explicit CBuffer(const uint* p)
        : d{{uintBitsToFloat(p[0]), uintBitsToFloat(p[1]), uintBitsToFloat(p[2]), uintBitsToFloat(p[3])}} {}
};
inline vec2 operator-(vec2 v) { return vec2(-v.x, -v.y); }
inline vec2 abs(vec2 v) { return vec2(std::abs(v.x), std::abs(v.y)); }
inline vec2 unpackHalf2x16(uint v) {
    return vec2(float(std::bit_cast<_Float16>(uint16_t(v))), float(std::bit_cast<_Float16>(uint16_t(v >> 16))));
}
inline uint bitfieldExtract(uint v, int off, int count) {
    if (off < 0 || count < 0 || off + count > 32) std::abort();
    if (!count) return 0;
    return (v >> off) & (count == 32 ? ~0u : (1u << count) - 1u);
}
inline int bitfieldExtract(int v, int off, int count) {
    uint x = bitfieldExtract(uint(v), off, count);
    if (count && count < 32 && (x & (1u << (count - 1)))) x |= ~0u << count;
    return int(x);
}
inline uint bitfieldInsert(uint base, uint insert, int off, int count) {
    if (off < 0 || count < 0 || off + count > 32) std::abort();
    if (!count) return base;
    uint mask = count == 32 ? ~0u : ((1u << count) - 1u) << off;
    return (base & ~mask) | ((insert << off) & mask);
}
struct State {
    uint r[256], predicates, flags, cbuf[4];
};
static_assert(sizeof(State) == 262 * sizeof(uint));
