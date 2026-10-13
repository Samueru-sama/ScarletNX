/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Derived from SwitchRecomp module.py, Copyright 2026 MZ Labs, Apache-2.0.
   Retain LICENSE.apache and NOTICE; the fork's changes are GPL-3.0-or-later. */
#include "module.h"
#include "codegen.h"
#include <stdlib.h>
#include <string.h>

static void hex(char* out, SnxU64 value) {
    if (value >> 32)
        sprintf(out, "0x%lX%08lX", (unsigned long)(value >> 32), (unsigned long)(value & 0xFFFFFFFFUL));
    else
        sprintf(out, "0x%lX", (unsigned long)value);
}

char* snx_chunk_label(const SnxCodegen* codegen, size_t first, size_t last) {
    char lo[24], hi[24];
    size_t i, instructions = 0;
    SnxU64 end = 0, address;
    const SnxFunction* f;
    if (first >= last) return snx_strdup("no functions");
    for (i = first; i < last; ++i) {
        f = &codegen->functions[i];
        instructions += f->count;
        address = f->count ? snx_function_address(f, f->count - 1) + 4 : f->start;
        if (address > end) end = address;
    }
    hex(lo, codegen->functions[first].start);
    hex(hi, end);
    return snx_format("%s-%s, %lu function%s, %lu instructions", lo, hi, (unsigned long)(last - first),
                      last - first == 1 ? "" : "s", (unsigned long)instructions);
}

void snx_report_unit(size_t done, size_t count, const char* name, const char* label, int cached, double seconds) {
    if (cached)
        printf("  compiled block %lu/%lu: %s, %s (cached)\n", (unsigned long)done, (unsigned long)count, name,
               label ? label : "");
    else
        printf("  compiled block %lu/%lu: %s, %s (%.1f s)\n", (unsigned long)done, (unsigned long)count, name,
               label ? label : "", seconds);
    fflush(stdout);
}

static int compare_entry(const void* a, const void* b) {
    const SnxModuleEntry *x, *y;
    x = (const SnxModuleEntry*)a;
    y = (const SnxModuleEntry*)b;
    if (x->address != y->address) return x->address < y->address ? -1 : 1;
    return x->function < y->function ? -1 : x->function > y->function;
}

SnxModuleEntry* snx_module_entries(const SnxCodegen* codegen, size_t* count) {
    SnxModuleEntry* entries;
    size_t n, i, j, unique;
    n = 0;
    for (i = 0; i < codegen->function_count; ++i) n += codegen->functions[i].leader_count;
    entries = (SnxModuleEntry*)snx_alloc((n ? n : 1) * sizeof(*entries));
    n = 0;
    for (i = 0; i < codegen->function_count; ++i)
        for (j = 0; j < codegen->functions[i].count; ++j)
            if (codegen->functions[i].leaders[j]) {
                entries[n].address = snx_function_address(&codegen->functions[i], j);
                entries[n++].function = i;
            }
    if (n) qsort(entries, n, sizeof(*entries), compare_entry);
    unique = 0;
    for (i = 0; i < n; ++i) {
        if (unique && entries[i].address == entries[unique - 1].address) continue;
        entries[unique++] = entries[i];
    }
    *count = unique;
    return entries;
}
