/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gpu/nvn_command_gap.h"

int nvn_command_gap_methods(const u32* words, u32 length, NvnGapMethodVisitor visit, void* context) {
    if (!words || !length || length > 16384) return 0;
    /* Preflight the entire gap before emitting any state. A truncated tail must not commit earlier binds. */
    for (u32 i = 0; i < length;) {
        u32 header = words[i], mode = header >> 29, count = (header >> 16) & 0x1fff;
        if (mode == 4) {
            i++;
            continue;
        }
        if ((mode != 1 && mode != 3 && mode != 5) || !count || count >= length - i) return 0;
        i += count + 1;
    }
    if (!visit) return 1;
    for (u32 i = 0; i < length;) {
        u32 header = words[i++], mode = header >> 29, sub = (header >> 13) & 7;
        u32 method = header & 0xfff, count = (header >> 16) & 0x1fff;
        if (mode == 4) {
            visit(sub, method, count, context);
            continue;
        }
        for (u32 k = 0; k < count; k++) {
            u32 actual_method = mode == 1 ? method + k : mode == 5 && k ? method + 1 : method;
            visit(sub, actual_method, words[i + k], context);
        }
        i += count;
    }
    return 1;
}

static void check_no_write(u32 subchannel, u32 method, u32 value, void* context) {
    int* valid = context;
    if (subchannel || !(method == 0x2 || (method >= 0x4 && method <= 0x6) ||
                        (method == 0x7 && ((value & 0x1f) == 1 || (value & 0x1f) == 4)) ||
                        (method >= 0x8e0 && method <= 0x8e3) ||
                        (method >= 0x904 && method <= 0x924 && ((method - 0x904) & 7) == 0) ||
                        method == 0x701 || method == 0x702 || method == 0x7c0 || method == 0x7c1 ||
                        (method >= 0x6c0 && method <= 0x6c2) ||
                        (method == 0x6c3 && (value & 3) == 1)))
        *valid = 0;
}

int nvn_command_gap_no_writes(const u32* words, u32 length) {
    int valid = 1;
    return nvn_command_gap_methods(words, length, check_no_write, &valid) && valid;
}

typedef struct {
    NvnGapReportState state;
    NvnGapReportWrite* writes;
    u32 count, capacity;
    u8 invalid;
} ReportScan;

static void scan_report_write(u32 subchannel, u32 method, u32 value, void* context) {
    ReportScan* scan = context;
    if (method == 0x4) { scan->state.puller_high = value; scan->state.puller_known |= 1; }
    else if (method == 0x5) { scan->state.puller_low = value; scan->state.puller_known |= 2; }
    else if (!subchannel && method == 0x6c0) { scan->state.query_high = value; scan->state.query_known |= 1; }
    else if (!subchannel && method == 0x6c1) { scan->state.query_low = value; scan->state.query_known |= 2; }
    else {
        int puller = method == 0x7 && (value & 0x1f) == 2;
        int query = !subchannel && method == 0x6c3 && ((value & 3) == 0 || (value & 3) == 2);
        if (!puller && !query) return;
        u8 known = puller ? scan->state.puller_known : scan->state.query_known;
        if (known != 3 || scan->count == scan->capacity) { scan->invalid = 1; return; }
        u32 high = puller ? scan->state.puller_high : scan->state.query_high;
        u32 low = puller ? scan->state.puller_low : scan->state.query_low;
        u32 size = puller ? ((value & (1u << 24)) ? 4 : 16)
                          : ((value & 3) == 0 ? ((value & ((1u << 4) | (1u << 28))) ? 4 : 16)
                                               : ((value & (1u << 28)) ? 4 : 16));
        if (scan->writes)
            scan->writes[scan->count] = (NvnGapReportWrite){((u64)(high & 0xff) << 32) | low, size, !!query};
        scan->count++;
    }
}

int nvn_command_gap_report_writes(const u32* words, u32 count, NvnGapReportState* state,
                                  NvnGapReportWrite* writes, u32 capacity, u32* write_count) {
    if (!state || !write_count || (capacity && !writes)) return 0;
    ReportScan scan = {.state = *state, .capacity = capacity};
    if (!nvn_command_gap_methods(words, count, scan_report_write, &scan) || scan.invalid) return 0;
    scan.state = *state;
    scan.writes = writes;
    scan.count = 0;
    if (!nvn_command_gap_methods(words, count, scan_report_write, &scan) || scan.invalid) return 0;
    *state = scan.state;
    *write_count = scan.count;
    return 1;
}

int nvn_command_gap_indexed_draw(const u32* words, u32 length, NvnGapIndexedDraw* out) {
    if (!words || !out || length < 6 || length > 16384) return 0;
    for (u32 i = 0; i < length;) {
        u32 header = words[i];
        u32 mode = header >> 29, sub = (header >> 13) & 7, method = header & 0xfff;
        u32 count = (header >> 16) & 0x1fff;
        if (mode == 4) {
            i++;
            continue;
        }
        if ((mode != 1 && mode != 3 && mode != 5) || !count || count >= length - i) return 0;
        if (mode == 5 && sub == 0 && method == 0xe38 && count == 5 && i + 6 == length) {
            u32 primitive = words[i + 1], high = words[i + 2], low = words[i + 3];
            u32 index_type = words[i + 4], vertices = words[i + 5];
            if (primitive > 6 || high >= 256 || index_type > 2 || !vertices || vertices > (1u << 24)) return 0;
            *out = (NvnGapIndexedDraw){((u64)high << 32) | low, primitive, index_type, vertices};
            return 1;
        }
        i += count + 1;
    }
    return 0;
}

typedef struct {
    u32 macro_method;
    NvnGapMacroData3 packet;
    u8 phase, found, invalid;
} MacroData3Scan;

static void scan_macro_data3(u32 subchannel, u32 method, u32 value, void* context) {
    MacroData3Scan* scan = context;
    if (scan->invalid) return;
    if (!subchannel && method == scan->macro_method) {
        if (scan->phase || scan->found) scan->invalid = 1;
        else {
            scan->packet.macro_argument = value;
            scan->phase = 1;
        }
        return;
    }
    if (!scan->phase) return;
    if (subchannel || method != 0x8e3 + scan->phase) {
        scan->invalid = 1;
        return;
    }
    scan->packet.words[scan->phase - 1] = value;
    if (++scan->phase == 4) {
        scan->found = 1;
        scan->phase = 0;
    }
}

int nvn_command_gap_macro_data3(const u32* words, u32 count, u32 macro_method, NvnGapMacroData3* out) {
    if (!out || macro_method < 0xe00 || macro_method > 0xfff) return 0;
    MacroData3Scan scan = {.macro_method = macro_method};
    if (!nvn_command_gap_methods(words, count, scan_macro_data3, &scan) ||
        !scan.found || scan.phase || scan.invalid) return 0;
    *out = scan.packet;
    return 1;
}
