/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Derived from SwitchRecomp module.py, Copyright 2026 MZ Labs, Apache-2.0.
   Retain LICENSE.apache and NOTICE; the fork's changes are GPL-3.0-or-later. */
#include "module.h"
#include "codegen.h"
#include "codegen_view.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    SnxU64 address;
    size_t function;
} Entry;

static void name(FILE* out, SnxU64 address) {
    fputs("F_", out);
    snx_a64_hex(out, address);
}
static void prototype(FILE* out, SnxU64 address) {
    fputs("void ", out);
    name(out, address);
    fputs("(CPU* RESTRICT c,u64 e);\n", out);
}

static void string_literal(FILE* out, const char* text) {
    const unsigned char* p;
    fputc('"', out);
    for (p = (const unsigned char*)text; *p; ++p) {
        if (*p == '"' || *p == '\\') {
            fputc('\\', out);
            fputc(*p, out);
        } else if (*p < 32 || *p >= 127)
            fprintf(out, "\\%03o", (unsigned)*p);
        else
            fputc(*p, out);
    }
    fputc('"', out);
}

static void reference(unsigned char* references, const SnxCodegen* codegen, SnxU64 address) {
    const SnxFunction* function;
    function = snx_codegen_function(codegen, address);
    if (function) references[function - codegen->functions] = 1;
}

static void emit_chunk(FILE* out, SnxCodegen* codegen, size_t first, size_t last) {
    unsigned char* references;
    size_t i, j;
    SnxU64 address;
    SnxU32 word;
    enum SnxFlow kind;
    SnxFunction* function;
    references = (unsigned char*)snx_alloc(codegen->function_count);
    memset(references, 0, codegen->function_count);
    for (i = first; i < last; ++i) {
        function = &codegen->functions[i];
        references[i] = 1;
        for (j = 0; j < function->count; ++j) {
            address = snx_function_address(function, j);
            if (!snx_codegen_word(codegen, address, &word)) continue;
            kind = snx_a64_decode(word)->kind;
            if (kind == SNX_FLOW_B || kind == SNX_FLOW_BL || kind == SNX_FLOW_BC)
                reference(references, codegen, snx_a64_target(word, address));
            if (!snx_function_contains(function, address + 4)) reference(references, codegen, address + 4);
        }
    }
    fputs("#include \"cpu.h\"\n", out);
    for (i = 0; i < codegen->function_count; ++i)
        if (references[i]) prototype(out, codegen->functions[i].start);
    for (i = first; i < last; ++i) snx_codegen_emit(out, codegen, &codegen->functions[i]);
    free(references);
}

static int compare_entry(const void* a, const void* b) {
    const Entry *x, *y;
    x = (const Entry*)a;
    y = (const Entry*)b;
    if (x->address != y->address) return x->address < y->address ? -1 : 1;
    return x->function < y->function ? -1 : x->function > y->function;
}

static Entry* make_entries(const SnxCodegen* codegen, size_t* count) {
    Entry* entries;
    size_t n, i, j, unique;
    n = 0;
    for (i = 0; i < codegen->function_count; ++i) n += codegen->functions[i].leader_count;
    entries = (Entry*)snx_alloc((n ? n : 1) * sizeof(*entries));
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

static int emit_module(FILE* out, SnxCodegen* codegen, const SnxCodegenOptions* options, const SnxProgram* loaded,
                       size_t* entry_count) {
    Entry* entries;
    size_t count, i, j, arraysize;
    SnxModuleImage* module;
    SnxU64 lo, hi, start, end;
    const SnxNpdm* npdm;
    entries = make_entries(codegen, &count);
    arraysize = count ? count : 1;
    for (i = 0; i < count; ++i)
        if (entries[i].address < SNX_CODE_BASE || entries[i].address - SNX_CODE_BASE > 0xFFFFFFFFUL) {
            free(entries);
            return snx_error("AOT entry exceeds the module ABI's 32-bit offset range");
        }
    fputs("#include \"cpu.h\"\n#include \"module_abi.h\"\n#include \"cpu_helpers.c\"\n", out);
    for (i = 0; i < codegen->function_count; ++i) prototype(out, codegen->functions[i].start);
    snx_a64_interp(out, 0);
    fputs("GuestFn snx_interp_entry;\n", out);
    fprintf(out, "static const GuestFn FNS[%lu]={",
            (unsigned long)(codegen->function_count ? codegen->function_count : 1));
    for (i = 0; i < codegen->function_count; ++i) {
        if (i) fputc(',', out);
        name(out, codegen->functions[i].start);
    }
    if (!codegen->function_count) fputc('0', out);
    fprintf(out, "};\nstatic const u32 ENT_OFF[%lu]={", (unsigned long)arraysize);
    for (i = 0; i < count; ++i)
        fprintf(out, "%s%lu", i ? "," : "", (unsigned long)(entries[i].address - SNX_CODE_BASE));
    if (!count) fputc('0', out);
    fprintf(out, "};\nstatic const u32 ENT_FN[%lu]={", (unsigned long)arraysize);
    for (i = 0; i < count; ++i) fprintf(out, "%s%lu", i ? "," : "", (unsigned long)entries[i].function);
    if (!count) fputc('0', out);
    fprintf(out, "};\nstatic u64 ENT_ADDR[%lu]; static GuestFn ENT_PTR[%lu];\n", (unsigned long)arraysize,
            (unsigned long)arraysize);
    fprintf(out, "static const SnxModInfo MODS[%lu]={\n", (unsigned long)loaded->module_count);
    lo = ~(SnxU64)0;
    hi = 0;
    for (i = 0; i < loaded->module_count; ++i) {
        module = &loaded->modules[i];
        if (i) fputs(",\n", out);
        fputc('{', out);
        string_literal(out, module->name);
        fputc(',', out);
        snx_a64_literal(out, module->base);
        fputc(',', out);
        snx_a64_literal(out, module->size);
        fputs(",{", out);
        for (j = 0; j < sizeof(module->build_id); ++j)
            fprintf(out, "%s%u", j ? "," : "", (unsigned)module->build_id[j]);
        fprintf(out, "},%lu,%lu,%lu,%lu,%lu,%lu,%lu,%u}", (unsigned long)module->text_off,
                (unsigned long)module->text_size, (unsigned long)module->ro_off, (unsigned long)module->ro_size,
                (unsigned long)module->data_off, (unsigned long)module->data_size, (unsigned long)module->bss,
                (unsigned)!!module->is_nro);
        start = module->base + module->text_off;
        end = start + module->text_size;
        if (start < lo) lo = start;
        if (end > hi) hi = end;
    }
    fputs("\n};\nstatic SnxModule MOD;\nSNX_EXPORT const SnxModule* snx_module(void){\n", out);
    fprintf(out, " u32 i; for(i=0;i<%lu;i++){ ENT_ADDR[i]=0x%lXULL+ENT_OFF[i]; ENT_PTR[i]=FNS[ENT_FN[i]]; }\n",
            (unsigned long)count, (unsigned long)SNX_CODE_BASE);
    fprintf(out, " snx_interp_entry=snx_interp; MOD.abi=%u; MOD.nmods=%lu; MOD.mods=MODS; MOD.nentries=%lu;\n",
            options->abi, (unsigned long)loaded->module_count, (unsigned long)count);
    fputs(" MOD.entry_addr=ENT_ADDR; MOD.entry_fn=ENT_PTR; MOD.interp=snx_interp; MOD.code_lo=", out);
    snx_a64_literal(out, lo);
    fputs("; MOD.code_hi=", out);
    snx_a64_literal(out, hi);
    fputs("; MOD.title=", out);
    string_literal(out, options->title ? options->title : "Game");
    fputs("; MOD.title_id=", out);
    npdm = &loaded->npdm;
    snx_a64_literal(out, npdm->title_id);
    fprintf(out, ";\n MOD.main_stack=%lu; MOD.main_prio=%u; MOD.main_core=%u; return &MOD; }\n",
            (unsigned long)npdm->stack, npdm->prio, npdm->core);
    free(entries);
    *entry_count = count;
    return 0;
}

static int finish_source(FILE* out, const char* temporary, const char* destination, size_t* total) {
    unsigned char* bytes;
    size_t size;
    int failed, result;
    failed = ferror(out);
    if (fclose(out)) failed = 1;
    if (failed) return snx_error("Writing generated C failed: %s", temporary);
    bytes = snx_read_file(temporary, &size);
    if (!bytes) return snx_error("Cannot read generated C: %s", temporary);
    result = snx_write_changed(destination, bytes, size);
    free(bytes);
    if (result) return snx_error("Cannot write generated C: %s", destination);
    *total += size;
    return 0;
}

int snx_generate_module(SnxProgram* loaded, const SnxCodegenOptions* options, SnxSources* sources) {
    SnxCodegen codegen;
    size_t first, last, count, chunk, total, entry_count, limit;
    char *temporary_dir, *temporary, *filename, *destination;
    FILE* out;
    int result;
    memset(sources, 0, sizeof(*sources));
    memset(&codegen, 0, sizeof(codegen));
    if (!loaded->module_count) return snx_error("No modules to generate");
    if (!options->interp_only &&
        snx_nx_codegen_analyze(&codegen, loaded, options->hints, options->hint_count, options->quiet))
        return -1;
    codegen.hook_direct_calls = options->hook_direct_calls;
    temporary_dir = snx_temp_dir(options->outdir);
    if (!temporary_dir) {
        snx_codegen_free(&codegen);
        return -1;
    }
    temporary = snx_path_join(temporary_dir, "source.c");
    sources->paths = (char**)snx_alloc((codegen.function_count + 1) * sizeof(*sources->paths));
    sources->paths[0] = snx_path_join(options->outdir, "module.c");
    sources->count = 1;
    result = -1;
    total = 0;
    chunk = 0;
    entry_count = 0;
    first = 0;
    limit = options->chunk_instrs ? options->chunk_instrs : 30000;
    while (first < codegen.function_count) {
        last = first;
        count = 0;
        do { count += codegen.functions[last++].count; } while (last < codegen.function_count && count < limit);
        filename = snx_format("code_%03lu.c", (unsigned long)chunk++);
        destination = snx_path_join(options->outdir, filename);
        free(filename);
        sources->paths[sources->count++] = destination;
        out = snx_fopen(temporary, "wb");
        if (!out) {
            snx_error("Cannot open generated C temporary file: %s", temporary);
            goto done;
        }
        emit_chunk(out, &codegen, first, last);
        if (finish_source(out, temporary, destination, &total)) goto done;
        if (!options->quiet && (chunk == 1 || chunk % 20 == 0))
            printf("  C generated: %lu chunks\n", (unsigned long)chunk);
        first = last;
    }
    out = snx_fopen(temporary, "wb");
    if (!out) {
        snx_error("Cannot open generated module temporary file: %s", temporary);
        goto done;
    }
    if (emit_module(out, &codegen, options, loaded, &entry_count)) {
        fclose(out);
        goto done;
    }
    if (finish_source(out, temporary, sources->paths[0], &total)) goto done;
    if (!options->quiet)
        printf("C code: %lu files, %.1f MB, %lu entries\n", (unsigned long)sources->count, (double)total / 1000000.0,
               (unsigned long)entry_count);
    result = 0;
done:
    snx_remove_tree(temporary_dir);
    free(temporary_dir);
    free(temporary);
    snx_codegen_free(&codegen);
    if (result) snx_sources_free(sources);
    return result;
}

void snx_sources_free(SnxSources* sources) {
    snx_free_strings(sources->paths, sources->count);
    memset(sources, 0, sizeof(*sources));
}
