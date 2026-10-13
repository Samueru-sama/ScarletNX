/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gpu/nvn_command_gap.h"
#include <assert.h>
#include <string.h>

typedef struct {
    u32 methods[12], values[12], count;
} CapturedMethods;

static void capture(u32 sub, u32 method, u32 value, void* context) {
    CapturedMethods* captured = context;
    assert(sub == 0 && captured->count < 12);
    captured->methods[captured->count] = method;
    captured->values[captured->count++] = value;
}

int main(void) {
    /* Observed TotK cursor gap: two state packets followed by its 1,656-index MME draw. */
    const u32 packet[] = {0x20020701, 6, 0x22ad0000, 0x200207c0, 6, 0x22ad0d8b,
                          0xa0050e38, 4, 6, 0x22ad0d90, 1, 1656};
    NvnGapIndexedDraw draw = {0};
    assert(nvn_command_gap_indexed_draw(packet, 12, &draw));
    assert(draw.primitive == 4 && draw.index_type == 1 && draw.count == 1656);
    assert(draw.index_address == 0x622ad0d90ull);

    CapturedMethods methods = {0};
    assert(nvn_command_gap_methods(packet, 12, capture, &methods));
    assert(methods.count == 9);
    assert(methods.methods[0] == 0x701 && methods.values[0] == 6);
    assert(methods.methods[1] == 0x702 && methods.values[1] == 0x22ad0000);
    assert(methods.methods[2] == 0x7c0 && methods.values[2] == 6);
    assert(methods.methods[3] == 0x7c1 && methods.values[3] == 0x22ad0d8b);
    assert(methods.methods[4] == 0xe38 && methods.values[4] == 4);
    assert(methods.methods[5] == 0xe39 && methods.values[5] == 6);
    assert(methods.methods[8] == 0xe39 && methods.values[8] == 1656);

    /* Increasing, non-increasing, and inline-value packets can share one validated gap. */
    const u32 state_packets[] = {0x200308e0, 0x100, 6, 0x22ad0000, 0x60010904, 0x31, 0x800108e3};
    methods.count = 0;
    assert(nvn_command_gap_methods(state_packets, 7, capture, &methods));
    assert(methods.count == 5);
    assert(methods.methods[0] == 0x8e0 && methods.values[0] == 0x100);
    assert(methods.methods[1] == 0x8e1 && methods.values[1] == 6);
    assert(methods.methods[2] == 0x8e2 && methods.values[2] == 0x22ad0000);
    assert(methods.methods[3] == 0x904 && methods.values[3] == 0x31);
    assert(methods.methods[4] == 0x8e3 && methods.values[4] == 1);
    const u32 binding_packets[] = {0x200308e0, 0x100, 6, 0x22ad0000,
                                   0x60010904, 0x31, 0x60010924, 0x41};
    assert(nvn_command_gap_buffer_bindings_only(binding_packets, 8));
    assert(!nvn_command_gap_buffer_bindings_only(state_packets, 7)); /* 0x8e3 is not classified. */
    assert(!nvn_command_gap_buffer_bindings_only(packet, 12)); /* Indexed draw writes attachments. */
    const u32 clear_packet[] = {0x60010674, 1};
    const u32 other_subchannel[] = {0x60012904, 0x31};
    assert(!nvn_command_gap_buffer_bindings_only(clear_packet, 2));
    assert(!nvn_command_gap_buffer_bindings_only(other_subchannel, 2));
    assert(!nvn_command_gap_buffer_bindings_only(binding_packets, 7));

    u32 altered[12];
    memcpy(altered, packet, sizeof altered);
    altered[11] = 3384;
    assert(nvn_command_gap_indexed_draw(altered, 12, &draw) && draw.count == 3384);
    memcpy(altered, packet, sizeof altered);
    altered[6] = 0xa0050e18; /* another MME method is not an indexed draw */
    assert(!nvn_command_gap_indexed_draw(altered, 12, &draw));
    memcpy(altered, packet, sizeof altered);
    altered[10] = 3; /* unsupported index width */
    assert(!nvn_command_gap_indexed_draw(altered, 12, &draw));
    memcpy(altered, packet, sizeof altered);
    altered[0] = 0x200d0701; /* a malformed prefix cannot hide a draw packet */
    assert(!nvn_command_gap_indexed_draw(altered, 12, &draw));
    methods.count = 0;
    assert(!nvn_command_gap_methods(altered, 12, capture, &methods) && !methods.count);
    assert(!nvn_command_gap_indexed_draw(packet, 11, &draw));
    assert(!nvn_command_gap_indexed_draw(NULL, 12, &draw));
    return 0;
}
