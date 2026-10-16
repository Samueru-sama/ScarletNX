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
        CHECK(frame.luma == uint64_t(0x1000 + job) << 8);
        CHECK(frame.chroma == uint64_t(0x2000 + job) << 8);
        CHECK(frame.previous_visible == (job == 1));
        picture[0x68] = job == 0 ? 0x12 : 0;
    }
    // A golden picture can remain unused while two other output surfaces alternate for many frames.
    // Track the decoder's slots from the emitted refresh flags, then ask for that old picture again.
    snx_vp9_free(builder);
    builder = snx_vp9_new();
    CHECK(builder);
    picture[0x68] = 1;
    regs[0x430 / 4 + 3] = 0x3000;
    CHECK(snx_vp9_compose(builder, picture.data(), probabilities.data(), body.data(), body.size(), regs.data(), &frame));
    std::array<uint32_t, 8> surfaces;
    surfaces.fill(0x3000);
    uint32_t last = 0x3000;
    for (unsigned job = 0; job < 20; ++job) {
        picture[0x68] = 0;
        for (unsigned ref = 0; ref < 3; ++ref) regs[0x430 / 4 + ref] = job == 19 ? 0x3000 : last;
        uint32_t output = 0x3001 + (job & 1);
        regs[0x430 / 4 + 3] = output;
        CHECK(snx_vp9_compose(builder, picture.data(), probabilities.data(), body.data(), body.size(), regs.data(), &frame));
        unsigned bit = 10; // marker/profile/show-existing/type/show/error (8), reset-frame-context (2)
        auto read = [&](unsigned n) {
            unsigned value = 0;
            while (n--) {
                value = (value << 1) | ((frame.data[bit / 8] >> (7 - bit % 8)) & 1);
                ++bit;
            }
            return value;
        };
        unsigned refresh = read(8);
        for (unsigned ref = 0; ref < 3; ++ref) {
            CHECK(surfaces[read(3)] == regs[0x430 / 4 + ref]);
            read(1); // reference sign bias
        }
        for (unsigned slot = 0; slot < surfaces.size(); ++slot)
            if (refresh & (1u << slot)) surfaces[slot] = output;
        last = output;
    }
    picture[0x7E] = 1; // one tile column is invalid for a 64-pixel frame
    CHECK(
        !snx_vp9_compose(builder, picture.data(), probabilities.data(), body.data(), body.size(), regs.data(), &frame));
    snx_vp9_free(builder);
    return 0;
}
