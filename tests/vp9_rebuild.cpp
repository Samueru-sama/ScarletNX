/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright 2026 the ScarletNX contributors */
#include "nx/gpu/video/vp9_rebuild.h"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

#define CHECK(expr)                                                                                                    \
    do {                                                                                                               \
        if (!(expr)) {                                                                                                 \
            std::fprintf(stderr, "vp9_rebuild.cpp:%d: check failed: %s\n", __LINE__, #expr);                           \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)

int main() {
    SnxVp9Builder* builder = snx_vp9_new();
    CHECK(builder);
    std::array<uint8_t, 0x100> picture{};
    std::array<uint8_t, 0xEA0> probabilities{};
    std::array<uint8_t, 16> body{};
    std::array<uint32_t, 0x1000 / 4> regs{};
    const uint32_t body_size = body.size();
    const int16_t dimensions[] = {64, 64, 64, 64};
    std::memcpy(picture.data() + 0x30, &body_size, sizeof body_size);
    std::memcpy(picture.data() + 0x60, dimensions, sizeof dimensions);
    picture[0x68] = 1;  // key frame
    picture[0x70] = 8;  // loop filter level
    picture[0x72] = 20; // quantizer
    probabilities.fill(128);
    SnxVp9Frame frame{};
    for (uint32_t job = 0; job < 3; ++job) {
        regs[0x430 / 4 + 3] = 0x1000 + job;
        regs[0x474 / 4 + 3] = 0x2000 + job;
        CHECK(snx_vp9_compose(builder, picture.data(), probabilities.data(), body.data(), body.size(), regs.data(),
                              &frame));
        CHECK(frame.size > body.size() && (frame.data[0] >> 6) == 2);
        CHECK(frame.width == 64 && frame.height == 64);
        CHECK(frame.luma == uint64_t(0x1000 + (job ? job - 1 : 0)) << 8);
        CHECK(frame.chroma == uint64_t(0x2000 + (job ? job - 1 : 0)) << 8);
        picture[0x68] = 0;
    }
    picture[0x7E] = 1; // one tile column is invalid for a 64-pixel frame
    CHECK(
        !snx_vp9_compose(builder, picture.data(), probabilities.data(), body.data(), body.size(), regs.data(), &frame));
    snx_vp9_free(builder);
    return 0;
}
