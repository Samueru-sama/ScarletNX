/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The translated float math keeps the console's order in every stage: the registers are precise, so the SPIR-V
   marks the arithmetic NoContraction and a driver can neither fuse nor reassociate it. Offline: the translator and
   glslang, no ICD or GPU. */
#include "gpu/shader/shader.h"
#include "gpu/vk/spirv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCHED    0x001F8000FC0007E0ull /* a control word: no waits */
#define EXIT     0xE30000000007000Full
#define FMUL     0x5C68000000070000ull /* FMUL R0, R0, R0 */
#define FMUL_IMM 0x3868003F00070000ull /* FMUL R0, R0, 0.5 (the immediate's top 19 bits, 0x3F000) */

static int failures;
static void check(int ok, const char* what) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

/* how many results the SPIR-V decorates NoContraction (OpDecorate, decoration 42) */
static int no_contraction(const u32* w, size_t n) {
    int count = 0;
    for (size_t i = 5; i < n;) {
        u32 len = w[i] >> 16, op = w[i] & 0xFFFF;
        if (!len || i + len > n) break;
        if (op == 71 && len >= 3 && w[i + 2] == 42) count++;
        i += len;
    }
    return count;
}

static void stage_keeps_order(int stage, const char* name) {
    static u64 code[64];
    memset(code, 0, sizeof code);
    code[10] = SCHED;
    code[11] = FMUL;
    code[12] = FMUL_IMM;
    code[13] = EXIT;
    ShaderInfo si;
    memset(&si, 0, sizeof si);
    char* glsl = shader_to_glsl((const u8*)code, sizeof code, stage, &si);
    check(glsl != NULL && !si.unsupported, name);
    if (!glsl) return;
    check(strstr(glsl, "precise uint r0") != NULL, name);
    size_t n = 0;
    char err[2048] = "";
    u32* spv = vk_spirv_cached(glsl, stage, NULL, &n, err, sizeof err);
    if (!spv) fprintf(stderr, "%s: %s\n%s\n", name, err, glsl);
    check(spv != NULL, name);
    if (spv) check(no_contraction(spv, n) >= 2, name);
    free(spv);
    free(glsl);
}

int main(void) {
    stage_keeps_order(ST_VS, "a vertex shader's float math keeps its order");
    stage_keeps_order(ST_FS, "a fragment shader's float math keeps its order");
    stage_keeps_order(ST_CS, "a compute shader's float math keeps its order");
    if (failures) return 1;
    puts("float math is precise (NoContraction) in vertex, fragment and compute shaders");
    return 0;
}
