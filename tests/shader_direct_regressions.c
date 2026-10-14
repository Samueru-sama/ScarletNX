/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gpu/shader/shader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCHED     0x001F8000FC0007E0ull
#define EXIT      0xE30000000007000Full
#define F2I       0x5CB0118000070A00ull
#define FMUL_FTZ  0x5C68100000070000ull
#define FMUL_KEEP 0x5C68000000070000ull
#define FADD_KEEP 0x5C58000000070000ull
#define FMUL_IMM  0x3868003F00070000ull
#define FFMA_FMZ  0x59C0000000070000ull
#define FFMA_KEEP 0x5980000000070000ull

static int failures;
static const char* storage_paths[2];
static const char* nro_paths[4];
static void check(int condition, const char* what) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

static u32* translate_with_rt_mask(const u64* instructions, int count, int stage, ShaderInfo* info, size_t* words,
                                   int flush, u32 rt_mask) {
    u64 code[64] = {0};
    int at = 10;
    for (int i = 0; i <= count; i++) {
        if ((at - 10) % 4 == 0) code[at++] = SCHED;
        code[at++] = i < count ? instructions[i] : EXIT;
    }
    memset(info, 0, sizeof *info);
    info->rt_int_mask = rt_mask;
    if (stage == ST_CS) {
        info->cs_local[0] = info->cs_local[1] = info->cs_local[2] = 1;
        info->shared_size = 16;
    }
    return shader_to_spirv((const u8*)code, sizeof code, stage, info, words, flush, 1, 1);
}
static u32* translate(const u64* instructions, int count, int stage, ShaderInfo* info, size_t* words, int flush) {
    return translate_with_rt_mask(instructions, count, stage, info, words, flush, 0);
}

static int count_opcode(const u32* code, size_t words, u32 opcode, u32 operand) {
    int found = 0;
    for (size_t i = 5; i < words;) {
        u32 width = code[i] >> 16, op = code[i] & 0xffff;
        if (!width || i + width > words) return -1;
        u32 value_index = op == 17 ? 1 : 2;
        if (op == opcode && (operand == 0xffffffffu || (width > value_index && code[i + value_index] == operand)))
            found++;
        i += width;
    }
    return found;
}

static void save_fixture(const char* path, const u32* code, size_t words) {
    if (!path || !code) return;
    FILE* out = fopen(path, "wb");
    check(out && fwrite(code, sizeof *code, words, out) == words, "shader fixture written");
    if (out) fclose(out);
}

static int has_glsl_abs(const u32* code, size_t words) {
    for (size_t i = 5; i < words;) {
        u32 width = code[i] >> 16;
        if (!width || i + width > words) return 0;
        if ((code[i] & 0xffff) == 12 && width >= 6 && code[i + 4] == 4) return 1; /* GLSL.std.450 FAbs */
        i += width;
    }
    return 0;
}

static int abs_feeds_negate(const u32* code, size_t words) {
    u32 abs_id = 0;
    for (size_t i = 5; i < words;) {
        u32 width = code[i] >> 16;
        if (!width || i + width > words) return 0;
        if ((code[i] & 0xffff) == 12 && width >= 6 && code[i + 4] == 4) abs_id = code[i + 2];
        if ((code[i] & 0xffff) == 127 && width >= 4 && code[i + 3] == abs_id && abs_id) return 1;
        i += width;
    }
    return 0;
}

static void f2i_modifiers(void) {
    const u64 cases[] = {F2I | (1ull << 45), F2I | (1ull << 49), F2I | (1ull << 45) | (1ull << 49)};
    for (int i = 0; i < 3; i++) {
        ShaderInfo info;
        size_t words = 0;
        u32* code = translate(&cases[i], 1, ST_FS, &info, &words, 1);
        check(code && !info.unsupported, "F2I modifier translates");
        if (code) {
            check((count_opcode(code, words, 127, 0xffffffffu) > 0) == (i != 1), "F2I bit 45 negates");
            check(has_glsl_abs(code, words) == (i != 0), "F2I bit 49 takes absolute value");
            if (i == 2) check(abs_feeds_negate(code, words), "F2I applies absolute value before negation");
        }
        free(code);
    }
}

static void global_buffers(void) {
    static const struct {
        u64 instructions[3];
        int count;
        u32 offset;
        int write;
    } cases[] = {
        {{0xEF9500007307FF00ull, 0xEED4200000070000ull}, 2, 0x730, 0},
        {{0x4C1080001CC70100ull, 0x4C1008001CD7FF01ull, 0xEEDC200000070002ull}, 3, 0x730, 1},
        {{0x4C98078014470002ull, 0xEED4200000070200ull}, 2, 0x510, 0},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        ShaderInfo info;
        size_t words = 0;
        u32* code = translate(cases[i].instructions, cases[i].count, ST_FS, &info, &words, 1);
        if (!code || info.unsupported || info.ngbuf != 1 || info.gbuf_cbuf_off[0] != cases[i].offset ||
            !!(info.gbuf_wmask & 1) != cases[i].write)
            fprintf(stderr, "global case %zu: %s, buffers %d, offset %x, writes %x\n", i, info.first_unsupported,
                    info.ngbuf, info.gbuf_cbuf_off[0], info.gbuf_wmask);
        check(code && !info.unsupported && info.ngbuf == 1 && info.gbuf_cbuf_off[0] == cases[i].offset &&
                  !!(info.gbuf_wmask & 1) == cases[i].write,
              "global buffer found at first use");
        free(code);
    }
    const u64 unused[] = {0x4C98078018870002ull};
    ShaderInfo info;
    size_t words = 0;
    u32* code = translate(unused, 1, ST_FS, &info, &words, 1);
    check(code && !info.unsupported && info.ngbuf == 0, "unused constant is not a global buffer");
    free(code);
}

static void float_controls(void) {
    const u64 cases[][2] = {{FMUL_FTZ, FMUL_FTZ},
                            {FMUL_FTZ, FADD_KEEP},
                            {FMUL_KEEP, FMUL_KEEP},
                            {FFMA_FMZ, FFMA_FMZ},
                            {FFMA_FMZ, FFMA_KEEP}};
    for (int i = 0; i < 5; i++) {
        for (int available = 0; available < 2; available++) {
            ShaderInfo info;
            size_t words = 0;
            u32* code = translate(cases[i], 2, ST_FS, &info, &words, available);
            check(code && !info.unsupported, "FP32 mode translates");
            if (code) {
                int flush = (i == 0 || i == 3) && available;
                check(info.fp32_flush == (i != 2) && info.fp32_preserve == (i == 1 || i == 2 || i == 4),
                      "FP32 modes recorded");
                check((count_opcode(code, words, 17, 4465) > 0) == flush, "FP32 flush capability");
                int mode = 0;
                for (size_t k = 5; k < words;) {
                    u32 width = code[k] >> 16;
                    if (!width || k + width > words) break;
                    if ((code[k] & 0xffff) == 16 && width >= 4 && code[k + 2] == 4460) mode = 1;
                    k += width;
                }
                check(mode == flush, "FP32 flush execution mode");
            }
            free(code);
        }
    }
}

static void precise_math(void) {
    const u64 math[] = {FMUL_KEEP, FMUL_IMM};
    const int stages[] = {ST_VS, ST_FS, ST_CS};
    for (size_t i = 0; i < sizeof stages / sizeof *stages; i++) {
        int stage = stages[i];
        ShaderInfo info;
        size_t words = 0;
        u32* code = translate(math, 2, stage, &info, &words, 1);
        if (!code || info.unsupported) fprintf(stderr, "precise stage %d: %s\n", stage, info.first_unsupported);
        check(code && !info.unsupported, "precise float math translates");
        if (code) check(count_opcode(code, words, 71, 42) >= 2, "float math has NoContraction");
        free(code);
    }
}

static void fragment_storage_images(void) {
    static const struct {
        u64 instructions[2];
        u8 kind;
        u32 opcode;
    } cases[] = {
        {{0x4C98078804A70002ull, 0xEB20010600F70004ull}, 2, 99},
        {{0x4C98078804A70001ull, 0xEB50008602070A00ull}, 1, 239},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        ShaderInfo info;
        size_t words = 0;
        u32* code = translate(cases[i].instructions, 2, ST_FS, &info, &words, 1);
        check(code && !info.unsupported && info.nimg == 1, "fragment storage image translates");
        if (code) {
            save_fixture(storage_paths[i], code, words);
            check(info.img[0].kind == cases[i].kind && info.img[0].bindless && info.img[0].bl_cbuf == 2 &&
                      info.img[0].cbuf_off == 74,
                  "fragment storage image resolves its handle and integer format");
            check(count_opcode(code, words, cases[i].opcode, 0xffffffffu) > 0,
                  "fragment storage image emits the write or atomic operation");
            if (cases[i].kind == 1)
                check(count_opcode(code, words, 60, 0xffffffffu) > 0, "image atomic uses a texel pointer");
        }
        free(code);
    }
    ShaderInfo alternate;
    size_t words = 0;
    u32* code = translate_with_rt_mask(cases[0].instructions, 2, ST_FS, &alternate, &words, 1, 1u << 8);
    check(code && !alternate.unsupported && alternate.nimg == 1 && alternate.img[0].kind == 0,
          "fragment surface store selects a float image from its runtime format key");
    free(code);
    const u64 clamped[] = {cases[1].instructions[0], cases[1].instructions[1] | (1ull << 49)};
    code = translate(clamped, 2, ST_FS, &alternate, &words, 1);
    check(!code && alternate.unsupported, "unsupported surface atomic clamp mode is rejected");
    free(code);
}

static u64 mov32i(u32 reg, u32 value) { return 0x0100000000000000ull | ((u64)value << 20) | 0x71000ull | reg; }

static int brx_table(u32 bank, u32 offset, u32* value) {
    if (bank || offset > 8) return 0;
    *value = offset == 0 ? 16 : 1024; /* second word belongs to another constant, not this jump table */
    return 1;
}

static int clamped_brx_table(u32 bank, u32 offset, u32* value) {
    if (bank || offset > 8) return 0;
    *value = offset < 8 ? 24 : 1024;
    return 1;
}

static int invalid_brx_table(u32 bank, u32 offset, u32* value) {
    if (bank || offset) return 0;
    *value = 1024;
    return 1;
}

static void brx_length(void) {
    u64 code[64] = {0};
    code[10] = 0x001F8000FC0007E0ull;
    code[11] = 0xef90000000070001ull; /* LDC R1, c[0][0] */
    code[12] = 0xe25000000007010full; /* BRX R1 */
    code[13] = 0xe30000000007000full;
    code[14] = code[10];
    code[15] = code[13];
    u32 targets[64], len = 0;
    int (*old)(u32, u32, u32*) = g_shader_cb_read;
    g_shader_cb_read = brx_table;
    shader_hash((const u8*)code, sizeof code, &len);
    int count = shader_brx_targets(code, 12, 64, targets, 64);
    check(count == 1 && targets[0] == 15, "BRX table stops at out-of-range trailing constant");
    check(len == 16 * 8, "BRX program length ends after its last reachable exit");
    g_shader_cb_read = invalid_brx_table;
    check(shader_brx_targets(code, 12, 64, targets, 64) == -1, "BRX rejects a table with no valid targets");
    memset(code, 0, sizeof code);
    code[10] = 0x001F8000FC0007E0ull;
    code[11] = 0x3820000000170b0bull; /* IMNMX R11, R11, 1: two entries */
    code[12] = 0x3848000000270b0dull; /* SHL R13, R11, 2 */
    code[13] = 0xef94000000070d0dull; /* LDC R13, c[0][R13] */
    code[14] = code[10];
    code[15] = 0xe250000000070d0full; /* BRX R13 */
    code[16] = 0xe30000000007000full;
    code[18] = code[10];
    code[19] = code[16];
    g_shader_cb_read = clamped_brx_table;
    shader_hash((const u8*)code, sizeof code, &len);
    count = shader_brx_targets(code, 15, 64, targets, 64);
    check(count == 2 && targets[0] == 19 && targets[1] == 19,
          "BRX reads only the entries permitted by its IMNMX clamp");
    check(len == 20 * 8, "clamped BRX bounds program length before padding");
    memset(code, 0, sizeof code);
    code[10] = 0x001F8000FC0007E0ull;
    code[11] = 0xe250000000070d0full; /* dynamic BRX with no readable jump table */
    code[12] = 0xe30000000007000full;
    g_shader_cb_read = NULL;
    shader_hash((const u8*)code, sizeof code, &len);
    check(len == 13 * 8, "unresolved BRX stops at the exit before an all-zero tail");
    code[15] = code[12];
    shader_hash((const u8*)code, sizeof code, &len);
    check(len == 16 * 8, "unresolved BRX keeps nonzero instructions beyond an earlier exit");
    g_shader_cb_read = old;
}

static void invalid_ldc_bank(void) {
    const u64 ldc = 0xef94000000070d0dull | (31ull << 36);
    ShaderInfo info;
    size_t words = 0;
    u32* code = translate(&ldc, 1, ST_FS, &info, &words, 1);
    check(!code && info.unsupported, "LDC rejects constant buffer banks outside the descriptor set");
    free(code);
}

static void nro_shader_modes(void) {
    const u64 raw[] = {0x4c98078800070004ull, mov32i(4, 1), mov32i(5, 3), 0xeb18048600470400ull};
    const u64 ms[] = {0x4c98078800070004ull, mov32i(5, 4), mov32i(0, 3), mov32i(1, 5), 0xdd3c0007a0470000ull};
    const u64 ms_array[] = {0x4c98078800070004ull, mov32i(5, 2), mov32i(0, 3),
                            mov32i(1, 5),          mov32i(2, 5), 0xdd3c0007b0470000ull};
    const u64 exchange[] = {mov32i(2, 1), mov32i(3, 2), 0xef5d00000007ff02ull, 0xec8000002027ff02ull};
    struct {
        const u64* instructions;
        int count;
        u8 flags;
    } cases[] = {{raw, 4, 1}, {ms, 5, 2}, {ms_array, 6, 2}, {exchange, 4, 0}};
    for (int i = 0; i < 4; i++) {
        ShaderInfo info = {0};
        info.shared_size = 16;
        size_t words = 0;
        u32* code = translate(cases[i].instructions, cases[i].count, ST_CS, &info, &words, 1);
        check(code && !info.unsupported, "NRO shader mode translates");
        if (code) {
            if (i < 3)
                check(info.ntex == 1 && (info.tex[0].flags & 3) == cases[i].flags, "NRO texture mode is recorded");
            save_fixture(nro_paths[i], code, words);
        }
        free(code);
    }
}

/* a rejected instruction is logged with its word */
static void unsupported_names_word(void) {
    ShaderInfo info;
    size_t words = 0;
    u32* code = translate((const u64[]){0xFFFFFFFFFFFFFFFFull}, 1, ST_FS, &info, &words, 0);
    check(!code && !strcmp(info.first_unsupported, "unknown opcode ffffffffffffffff"), "an unknown opcode is named");
    free(code);
    code = translate((const u64[]){0xEB10030200480502ull}, 1, ST_CS, &info, &words, 0);
    check(!code && !strcmp(info.first_unsupported, "SULD eb10030200480502"), "a SULD form not done yet is named");
    free(code);
}

int main(int argc, char** argv) {
    if (argc != 1 && argc != 7) return 2;
    if (argc == 7) {
        storage_paths[0] = argv[1];
        storage_paths[1] = argv[2];
        for (int i = 0; i < 4; i++) nro_paths[i] = argv[i + 3];
    }
    brx_length();
    invalid_ldc_bank();
    unsupported_names_word();
    f2i_modifiers();
    global_buffers();
    float_controls();
    precise_math();
    fragment_storage_images();
    nro_shader_modes();
    return failures ? 1 : 0;
}
