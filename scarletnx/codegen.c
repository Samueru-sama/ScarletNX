/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Derived from SwitchRecomp codegen.py, Copyright 2026 MZ Labs, Apache-2.0.
   Retain LICENSE.apache and NOTICE; the fork's changes are GPL-3.0-or-later. */
#include "codegen.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    SnxU64* values;
    size_t count, capacity;
} Addresses;
typedef struct {
    SnxU64* keys;
    unsigned char* used;
    size_t count, capacity;
} AddressSet;
typedef struct {
    SnxU64 start, end;
    size_t ordinal;
} Range;

static void push(Addresses* a, SnxU64 value) {
    if (a->count == a->capacity) {
        a->capacity = a->capacity ? a->capacity * 2 : 64;
        a->values = (SnxU64*)snx_realloc(a->values, a->capacity * sizeof(*a->values));
    }
    a->values[a->count++] = value;
}

static size_t address_hash(SnxU64 value) { return (size_t)((value ^ (value >> 32)) * 2654435761UL); }

static int set_has(const AddressSet* set, SnxU64 value) {
    size_t i;
    if (!set->capacity) return 0;
    i = address_hash(value) & (set->capacity - 1);
    while (set->used[i]) {
        if (set->keys[i] == value) return 1;
        i = (i + 1) & (set->capacity - 1);
    }
    return 0;
}

static int set_add(AddressSet* set, SnxU64 value) {
    AddressSet old;
    size_t i;
    if (!set->capacity || (set->count + 1) * 4 >= set->capacity * 3) {
        old = *set;
        set->capacity = old.capacity ? old.capacity * 2 : 128;
        set->count = 0;
        set->keys = (SnxU64*)snx_alloc(set->capacity * sizeof(*set->keys));
        set->used = (unsigned char*)snx_alloc(set->capacity);
        memset(set->used, 0, set->capacity);
        for (i = 0; i < old.capacity; ++i)
            if (old.used[i]) set_add(set, old.keys[i]);
        free(old.keys);
        free(old.used);
    }
    i = address_hash(value) & (set->capacity - 1);
    while (set->used[i]) {
        if (set->keys[i] == value) return 0;
        i = (i + 1) & (set->capacity - 1);
    }
    set->used[i] = 1;
    set->keys[i] = value;
    ++set->count;
    return 1;
}

static void set_free(AddressSet* set) {
    free(set->keys);
    free(set->used);
    memset(set, 0, sizeof(*set));
}
static int compare_address(const void* a, const void* b) {
    SnxU64 x, y;
    x = *(const SnxU64*)a;
    y = *(const SnxU64*)b;
    return x < y ? -1 : x > y;
}
static int compare_function(const void* a, const void* b) {
    const SnxFunction *x, *y;
    x = (const SnxFunction*)a;
    y = (const SnxFunction*)b;
    return x->start < y->start ? -1 : x->start > y->start;
}
static int compare_range(const void* a, const void* b) {
    const Range *x, *y;
    x = (const Range*)a;
    y = (const Range*)b;
    if (x->start != y->start) return x->start < y->start ? -1 : 1;
    return x->ordinal > y->ordinal ? -1 : x->ordinal < y->ordinal;
}

SnxModuleImage* snx_codegen_module(SnxCodegen* codegen, SnxU64 address) {
    size_t i;
    SnxModuleImage* module;
    SnxU64 start;
    module = codegen->last_module;
    if (module) {
        start = module->base + module->text_off;
        if (address >= start && address - start < module->text_size) return module;
    }
    for (i = 0; i < codegen->loaded->module_count; ++i) {
        module = &codegen->loaded->modules[i];
        start = module->base + module->text_off;
        if (address >= start && address - start < module->text_size) {
            codegen->last_module = module;
            return module;
        }
    }
    return NULL;
}

int snx_codegen_word(SnxCodegen* codegen, SnxU64 address, SnxU32* word) {
    SnxModuleImage* module;
    size_t off;
    const unsigned char* p;
    module = snx_codegen_module(codegen, address);
    if (!module || (address & 3)) return 0;
    off = (size_t)(address - module->base);
    if (off > module->image_size || module->image_size - off < 4 ||
        address - (module->base + module->text_off) + 4 > module->text_size)
        return 0;
    p = module->image + off;
    *word = (SnxU32)p[0] | ((SnxU32)p[1] << 8) | ((SnxU32)p[2] << 16) | ((SnxU32)p[3] << 24);
    return 1;
}

SnxU64 snx_function_address(const SnxFunction* function, size_t index) {
    return function->addresses ? function->addresses[index] : function->start + (SnxU64)index * 4;
}

static size_t function_index(const SnxFunction* function, SnxU64 address) {
    size_t lo, hi, middle;
    if (!function->addresses) {
        if (address < function->start || address >= function->end || ((address - function->start) & 3))
            return function->count;
        return (size_t)((address - function->start) / 4);
    }
    lo = 0;
    hi = function->count;
    while (lo < hi) {
        middle = lo + (hi - lo) / 2;
        if (function->addresses[middle] < address)
            lo = middle + 1;
        else
            hi = middle;
    }
    return lo < function->count && function->addresses[lo] == address ? lo : function->count;
}

int snx_function_contains(const SnxFunction* function, SnxU64 address) {
    return function_index(function, address) < function->count;
}

const SnxFunction* snx_codegen_function(const SnxCodegen* codegen, SnxU64 address) {
    size_t lo, hi, middle;
    lo = 0;
    hi = codegen->function_count;
    while (lo < hi) {
        middle = lo + (hi - lo) / 2;
        if (codegen->functions[middle].start < address)
            lo = middle + 1;
        else
            hi = middle;
    }
    return lo < codegen->function_count && codegen->functions[lo].start == address ? &codegen->functions[lo] : NULL;
}

static const Range* inside_range(const Range* ranges, size_t count, SnxU64 address) {
    size_t lo, hi, middle;
    lo = 0;
    hi = count;
    while (lo < hi) {
        middle = lo + (hi - lo) / 2;
        if (ranges[middle].start <= address)
            lo = middle + 1;
        else
            hi = middle;
    }
    return lo && address < ranges[lo - 1].end ? &ranges[lo - 1] : NULL;
}

static void add_function(SnxCodegen* codegen, SnxU64 start, SnxU64 end, Addresses* addresses) {
    SnxFunction* f;
    if (codegen->function_count == codegen->function_capacity) {
        codegen->function_capacity = codegen->function_capacity ? codegen->function_capacity * 2 : 128;
        codegen->functions =
            (SnxFunction*)snx_realloc(codegen->functions, codegen->function_capacity * sizeof(*codegen->functions));
    }
    f = &codegen->functions[codegen->function_count++];
    memset(f, 0, sizeof(*f));
    f->start = start;
    f->end = end;
    if (addresses) {
        f->addresses = addresses->values;
        f->count = addresses->count;
        memset(addresses, 0, sizeof(*addresses));
    } else
        f->count = (size_t)((end - start + 3) / 4);
}

static Addresses explore(SnxCodegen* codegen, SnxU64 start, const AddressSet* known, Addresses* work) {
    Addresses result, todo;
    AddressSet seen;
    SnxU64 address, target, distance;
    SnxU32 word;
    enum SnxFlow kind;
    memset(&result, 0, sizeof(result));
    memset(&todo, 0, sizeof(todo));
    memset(&seen, 0, sizeof(seen));
    push(&todo, start);
    while (todo.count && seen.count <= 200000) {
        address = todo.values[--todo.count];
        for (;;) {
            if (set_has(&seen, address) || (address != start && set_has(known, address))) break;
            if (!snx_codegen_word(codegen, address, &word)) break;
            set_add(&seen, address);
            push(&result, address);
            kind = snx_a64_decode(word)->kind;
            if (kind == SNX_FLOW_UNDEF) break;
            if (kind == SNX_FLOW_BC || kind == SNX_FLOW_B || kind == SNX_FLOW_BL) {
                target = snx_a64_target(word, address);
                if (kind == SNX_FLOW_BC) {
                    if (snx_codegen_module(codegen, target)) push(&todo, target);
                } else if (kind == SNX_FLOW_B) {
                    distance = target > start ? target - start : start - target;
                    if (snx_codegen_module(codegen, target) && !set_has(known, target) && distance < 0x100000UL)
                        push(&todo, target);
                    break;
                } else if (work && snx_codegen_module(codegen, target))
                    push(work, target);
            } else if (kind == SNX_FLOW_BR || kind == SNX_FLOW_RET || kind == SNX_FLOW_BRK)
                break;
            address += 4;
            if (seen.count > 200000) break;
        }
    }
    if (result.count) qsort(result.values, result.count, sizeof(*result.values), compare_address);
    free(todo.values);
    set_free(&seen);
    return result;
}

static void mark_leader(SnxFunction* function, SnxU64 address) {
    size_t index;
    index = function_index(function, address);
    if (index < function->count && !function->leaders[index]) {
        function->leaders[index] = 1;
        ++function->leader_count;
    }
}

static void leaders(SnxCodegen* codegen, const AddressSet* extra, const AddressSet* hints) {
    size_t i, j;
    SnxFunction* f;
    SnxU64 address;
    SnxU32 word;
    enum SnxFlow kind;
    int previous_terminal;
    for (i = 0; i < codegen->function_count; ++i) {
        f = &codegen->functions[i];
        f->leaders = (unsigned char*)snx_alloc(f->count);
        memset(f->leaders, 0, f->count);
        mark_leader(f, f->start);
        previous_terminal = 0;
        for (j = 0; j < f->count; ++j) {
            address = snx_function_address(f, j);
            if (previous_terminal || set_has(extra, address) || set_has(hints, address)) mark_leader(f, address);
            kind = snx_codegen_word(codegen, address, &word) ? snx_a64_decode(word)->kind : SNX_FLOW_UNDEF;
            if (kind == SNX_FLOW_B || kind == SNX_FLOW_BC) mark_leader(f, snx_a64_target(word, address));
            if (kind == SNX_FLOW_BL || kind == SNX_FLOW_BLR) mark_leader(f, address + 4);
            previous_terminal = kind == SNX_FLOW_B || kind == SNX_FLOW_BR || kind == SNX_FLOW_RET ||
                                kind == SNX_FLOW_BRK || kind == SNX_FLOW_UNDEF;
        }
        codegen->instruction_count += f->count;
    }
}

int snx_codegen_analyze(SnxCodegen* codegen, SnxProgram* loaded, const SnxU64* hints, size_t hint_count, int quiet) {
    Range* ranges;
    const Range* range;
    size_t nranges, capranges, i, j, covered, round, pending_start, pending_end, next_start;
    Addresses work, found, fresh;
    AddressSet known, seen_seed, extra, hint_set, seeds, fde_starts;
    SnxModuleImage* m;
    SnxU64 a, end, text_end, target;
    SnxU32 word;
    enum SnxFlow kind;
    SnxFunction* f;
    memset(codegen, 0, sizeof(*codegen));
    codegen->loaded = loaded;
    memset(&work, 0, sizeof(work));
    memset(&known, 0, sizeof(known));
    memset(&seen_seed, 0, sizeof(seen_seed));
    memset(&extra, 0, sizeof(extra));
    memset(&hint_set, 0, sizeof(hint_set));
    memset(&seeds, 0, sizeof(seeds));
    memset(&fde_starts, 0, sizeof(fde_starts));
    ranges = NULL;
    nranges = 0;
    capranges = 0;
    for (i = 0; i < loaded->module_count; ++i) {
        m = &loaded->modules[i];
        text_end = m->base + m->text_off + m->text_size;
        for (j = 0; j < m->fde_count; ++j) {
            a = m->base + m->fde[j].start;
            if (a < m->base + m->text_off || a >= text_end || !m->fde[j].size || (a & 3)) continue;
            set_add(&fde_starts, a);
            end = m->fde[j].size > text_end - a ? text_end : a + m->fde[j].size;
            if (nranges == capranges) {
                capranges = capranges ? capranges * 2 : 128;
                ranges = (Range*)snx_realloc(ranges, capranges * sizeof(*ranges));
            }
            ranges[nranges].start = a;
            ranges[nranges].end = end;
            ranges[nranges].ordinal = nranges;
            ++nranges;
        }
        for (j = 0; j < m->function_count; ++j)
            if (set_add(&seeds, m->base + m->functions[j])) push(&work, m->base + m->functions[j]);
        for (j = 0; j < m->relptr_count; ++j)
            if (set_add(&seeds, m->base + m->relptrs[j])) push(&work, m->base + m->relptrs[j]);
        if (set_add(&seeds, m->base + m->text_off)) push(&work, m->base + m->text_off);
    }
    for (i = 0; i < hint_count; ++i) {
        set_add(&hint_set, hints[i]);
        if (snx_codegen_module(codegen, hints[i]) && set_add(&seeds, hints[i])) push(&work, hints[i]);
    }
    /* Symbols/hints at an FDE start do not become recursive seeds, including
       FDEs later discarded because an earlier range covers them. */
    covered = 0;
    for (i = 0; i < work.count; ++i)
        if (!set_has(&fde_starts, work.values[i])) work.values[covered++] = work.values[i];
    work.count = covered;
    if (nranges) qsort(ranges, nranges, sizeof(*ranges), compare_range);
    covered = 0;
    end = 0;
    for (i = 0; i < nranges; ++i) {
        if (ranges[i].start < end) continue;
        ranges[covered++] = ranges[i];
        end = ranges[i].end;
        add_function(codegen, ranges[i].start, end, NULL);
        set_add(&known, ranges[i].start);
    }
    nranges = covered;
    while (work.count) {
        a = work.values[--work.count];
        if ((a & 3) || !set_add(&seen_seed, a)) continue;
        range = inside_range(ranges, nranges, a);
        if (range) {
            if (a != range->start) set_add(&extra, a);
            continue;
        }
        if (set_has(&known, a) || !snx_codegen_module(codegen, a)) continue;
        found = explore(codegen, a, &known, &work);
        if (found.count) {
            add_function(codegen, a, 0, &found);
            set_add(&known, a);
        }
        free(found.values);
    }
    pending_start = 0;
    pending_end = codegen->function_count;
    for (round = 0; round < 8 && pending_start < pending_end; ++round) {
        memset(&fresh, 0, sizeof(fresh));
        for (i = pending_start; i < pending_end; ++i) {
            f = &codegen->functions[i];
            for (j = 0; j < f->count; ++j) {
                a = snx_function_address(f, j);
                if (!snx_codegen_word(codegen, a, &word)) continue;
                kind = snx_a64_decode(word)->kind;
                if (kind != SNX_FLOW_BL && kind != SNX_FLOW_B && kind != SNX_FLOW_BC) continue;
                target = snx_a64_target(word, a);
                if (set_has(&known, target) || set_has(&seen_seed, target) || !snx_codegen_module(codegen, target))
                    continue;
                if (kind != SNX_FLOW_BL && snx_function_contains(f, target)) continue;
                range = inside_range(ranges, nranges, target);
                if (range) {
                    if (kind == SNX_FLOW_BL && target != range->start) set_add(&extra, target);
                    continue;
                }
                if (kind == SNX_FLOW_BL || target < f->start || target > snx_function_address(f, f->count - 1))
                    push(&fresh, target);
            }
        }
        next_start = codegen->function_count;
        for (i = 0; i < fresh.count; ++i) {
            a = fresh.values[i];
            if (set_has(&known, a) || !set_add(&seen_seed, a)) continue;
            found = explore(codegen, a, &known, NULL);
            if (found.count) {
                add_function(codegen, a, 0, &found);
                set_add(&known, a);
            }
            free(found.values);
        }
        free(fresh.values);
        pending_start = next_start;
        pending_end = codegen->function_count;
    }
    if (codegen->function_count)
        qsort(codegen->functions, codegen->function_count, sizeof(*codegen->functions), compare_function);
    leaders(codegen, &extra, &hint_set);
    if (!quiet)
        printf("Functions: %lu  instructions: %lu\n", (unsigned long)codegen->function_count,
               (unsigned long)codegen->instruction_count);
    free(work.values);
    free(ranges);
    set_free(&known);
    set_free(&seen_seed);
    set_free(&extra);
    set_free(&hint_set);
    set_free(&seeds);
    set_free(&fde_starts);
    return 0;
}

void snx_codegen_free(SnxCodegen* codegen) {
    size_t i;
    for (i = 0; i < codegen->function_count; ++i) {
        free(codegen->functions[i].addresses);
        free(codegen->functions[i].leaders);
    }
    free(codegen->functions);
    memset(codegen, 0, sizeof(*codegen));
}

static void function_name(FILE* out, SnxU64 address) {
    fputs("F_", out);
    snx_a64_hex(out, address);
}
static void label(FILE* out, SnxU64 address) {
    fputs("L_", out);
    snx_a64_hex(out, address);
}

static void call(FILE* out, const SnxCodegen* codegen, SnxU64 target, int tail) {
    const SnxFunction* function;
    function = snx_codegen_function(codegen, target);
    if (tail) fputs("{ ", out);
    if (function && !codegen->hook_direct_calls) {
        function_name(out, target);
        fputs("(c,", out);
    } else
        fputs("CALLDYN(", out);
    snx_a64_literal(out, target);
    fputs(");", out);
    if (tail) fputs(" return; }", out);
}

static void branch(FILE* out, const SnxCodegen* codegen, const SnxFunction* function, SnxU64 target) {
    if (snx_function_contains(function, target)) {
        fputs("goto ", out);
        label(out, target);
        fputc(';', out);
    } else
        call(out, codegen, target, 1);
}

void snx_codegen_emit(FILE* out, SnxCodegen* codegen, const SnxFunction* function) {
    size_t i;
    SnxU64 address, next, target;
    SnxU32 word;
    const SnxA64Case* c;
    int dispatch, terminal;
    fputs("void ", out);
    function_name(out, function->start);
    fputs("(CPU* RESTRICT c,u64 e){\n u8* const M=c->mem;\n", out);
    fputs(" if(UNLIKELY(e!=", out);
    snx_a64_literal(out, function->start);
    dispatch = function->leader_count > 1;
    fputs(dispatch ? ")){ c->pc=e; goto D_; }\n" : ")){ CALLDYN(e); return; }\n", out);
    if (function->count && snx_function_address(function, 0) != function->start) {
        fputs(" goto ", out);
        label(out, function->start);
        fputs(";\n", out);
    }
    for (i = 0; i < function->count; ++i) {
        address = snx_function_address(function, i);
        next = address + 4;
        if (function->leaders[i]) {
            label(out, address);
            fputs(":;\n", out);
        }
        word = 0;
        if (!snx_codegen_word(codegen, address, &word))
            c = snx_a64_decode(0);
        else
            c = snx_a64_decode(word);
        if (c->kind == SNX_FLOW_UNDEF) {
            fputs(" c->pc=", out);
            snx_a64_literal(out, address);
            fputs("; c->rt->undef(c,", out);
            snx_a64_literal(out, address);
            fprintf(out, ",0x%08lXu); return;\n", (unsigned long)word);
            continue;
        }
        terminal = 0;
        snx_a64_scope(out, word, address, c);
        switch (c->kind) {
            case SNX_FLOW_PLAIN:
                if (!snx_a64_fast_simd(out, word)) snx_a64_body(out, c->first, "c->rt->undef(c,pc,w)");
                break;
            case SNX_FLOW_B:
                branch(out, codegen, function, snx_a64_target(word, address));
                terminal = 1;
                break;
            case SNX_FLOW_BC:
                fprintf(out, "if(%s) ", c->first);
                branch(out, codegen, function, snx_a64_target(word, address));
                break;
            case SNX_FLOW_BL:
                target = snx_a64_target(word, address);
                fputs("c->x[30]=pc+4; ", out);
                call(out, codegen, target, 0);
                fputs(" if(UNLIKELY(c->pc!=pc+4)) return;", out);
                break;
            case SNX_FLOW_BLR:
                fprintf(out, "{ u64 t_=%s; c->x[30]=pc+4; CALLDYN(t_); if(UNLIKELY(c->pc!=pc+4)) return; }", c->first);
                break;
            case SNX_FLOW_BR:
                if (dispatch)
                    fprintf(out, "c->pc=%s; goto D_;", c->first);
                else
                    fprintf(out, "{ u64 t_=%s; CALLDYN(t_); return; }", c->first);
                terminal = 1;
                break;
            case SNX_FLOW_RET:
                fprintf(out, "c->pc=%s; return;", c->first);
                terminal = 1;
                break;
            case SNX_FLOW_SVC: fprintf(out, "c->pc=pc+4; c->rt->svc(c,%s);", c->first); break;
            case SNX_FLOW_BRK:
                fprintf(out, "c->pc=pc; c->rt->brk(c,pc,%s); return;", c->first);
                terminal = 1;
                break;
            case SNX_FLOW_UNDEF: break;
        }
        fputs(" }\n", out);
        if (!terminal && (!snx_function_contains(function, next) ||
                          (i + 1 < function->count && snx_function_address(function, i + 1) != next))) {
            fputc(' ', out);
            call(out, codegen, next, 1);
            fputc('\n', out);
        }
    }
    if (dispatch) {
        fputs(" D_: switch(c->pc){\n", out);
        for (i = 0; i < function->count; ++i)
            if (function->leaders[i]) {
                address = snx_function_address(function, i);
                fputs(" case ", out);
                snx_a64_literal(out, address);
                fputs(": goto ", out);
                label(out, address);
                fputs(";\n", out);
            }
        fputs(" default: CALLDYN(c->pc); return; }\n", out);
    }
    fputs("}\n", out);
}
