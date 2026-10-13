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
    assert(nvn_command_gap_no_writes(binding_packets, 8));
    const u32 other_stage_bindings[] = {0x6001090c, 0x31, 0x60010914, 0x41, 0x6001091c, 0x51};
    assert(nvn_command_gap_no_writes(other_stage_bindings, 6));
    assert(nvn_command_gap_no_writes(state_packets, 7)); /* CBUF offset is register state. */
    const u32 vertex_range[] = {0x20020701, 6, 0x22ad0000, 0x200207c0, 6, 0x22ad0d8b};
    assert(nvn_command_gap_no_writes(vertex_range, 6));
    assert(!nvn_command_gap_no_writes(packet, 12)); /* Indexed draw writes attachments. */
    const u32 clear_packet[] = {0x60010674, 1};
    const u32 upload_packet[] = {0x200408e1, 6, 0x22ad0000, 0, 0x12345678};
    const u32 report_packet[] = {0x200406c0, 6, 0x22ad0000, 1, 0};
    const u32 acquire_packet[] = {0x200406c0, 6, 0x22ad0000, 1, 1};
    const u32 counter_packet[] = {0x200406c0, 6, 0x22ad0000, 1, 2};
    const u32 puller_acquire[] = {0x20040004, 6, 0x22ad0000, 1, 1};
    const u32 puller_release[] = {0x20040004, 6, 0x22ad0000, 1, 2};
    const u32 other_subchannel[] = {0x60012904, 0x31};
    assert(!nvn_command_gap_no_writes(clear_packet, 2));
    assert(!nvn_command_gap_no_writes(upload_packet, 5)); /* CBUF data method writes guest memory. */
    assert(!nvn_command_gap_no_writes(report_packet, 5)); /* Query release writes guest memory. */
    assert(nvn_command_gap_no_writes(acquire_packet, 5)); /* Query acquire reads the semaphore. */
    assert(!nvn_command_gap_no_writes(counter_packet, 5));
    assert(nvn_command_gap_no_writes(puller_acquire, 5));
    assert(!nvn_command_gap_no_writes(puller_release, 5));
    NvnGapReportState report_state = {0};
    NvnGapReportWrite writes[2] = {0};
    u32 write_count = 99;
    assert(!nvn_command_gap_report_writes(report_packet, 5, &report_state, writes, 0, &write_count));
    assert(write_count == 99 && !report_state.query_known);
    assert(nvn_command_gap_report_writes(report_packet, 5, &report_state, writes, 2, &write_count));
    assert(write_count == 1 && writes[0].query && writes[0].address == 0x622ad0000ull && writes[0].size == 16);
    assert(nvn_command_gap_report_writes(acquire_packet, 5, &report_state, writes, 2, &write_count));
    assert(write_count == 0);
    const u32 puller_address[] = {0x20030004, 6, 0x22ad0000, 1};
    const u32 puller_word_release[] = {0x60010007, 0x01000002};
    assert(nvn_command_gap_report_writes(puller_address, 4, &report_state, writes, 2, &write_count));
    assert(write_count == 0 && report_state.puller_known == 3);
    assert(nvn_command_gap_report_writes(puller_word_release, 2, &report_state, writes, 2, &write_count));
    assert(write_count == 1 && !writes[0].query && writes[0].address == 0x622ad0000ull && writes[0].size == 4);
    assert(nvn_command_gap_report_writes(counter_packet, 5, &report_state, writes, 2, &write_count));
    assert(write_count == 1 && writes[0].query && writes[0].size == 16);
    const u32 two_releases[] = {0x60010007, 2, 0x60010007, 2};
    NvnGapReportState before_overflow = report_state;
    assert(!nvn_command_gap_report_writes(two_releases, 4, &report_state, writes, 1, &write_count));
    assert(!memcmp(&report_state, &before_overflow, sizeof report_state));
    assert(!nvn_command_gap_no_writes(other_subchannel, 2));
    assert(!nvn_command_gap_no_writes(binding_packets, 7));

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

    /* TotK's descriptor upload: the MME argument is a word offset, but that interpretation belongs
       to its uploaded macro program and is not inferred by this packet parser. */
    const u32 descriptor_packet[] = {
        0x60010e0e, 4, 0x6001068b, 0, 0x600108e3, 0xa8, 0x600108e4, 0x14d0103c,
        0x60010e20, 0x154, 0x200308e4, 0x23130000, 6, 0xe5e0,
    };
    assert(!nvn_command_gap_no_writes(descriptor_packet, 14)); /* CBUF data upload writes memory. */
    NvnGapMacroData3 descriptor = {0};
    assert(nvn_command_gap_macro_data3(descriptor_packet, 14, 0xe20, &descriptor));
    assert(descriptor.macro_argument == 0x154);
    assert(descriptor.words[0] == 0x23130000 && descriptor.words[1] == 6 && descriptor.words[2] == 0xe5e0);
    assert(!nvn_command_gap_macro_data3(descriptor_packet, 14, 0xe18, &descriptor));
    u32 interrupted[14];
    memcpy(interrupted, descriptor_packet, sizeof interrupted);
    interrupted[10] = 0x200308e3; /* An offset setter in place of the first data word breaks the sequence. */
    assert(!nvn_command_gap_macro_data3(interrupted, 14, 0xe20, &descriptor));
    assert(!nvn_command_gap_macro_data3(descriptor_packet, 13, 0xe20, &descriptor));
    return 0;
}
