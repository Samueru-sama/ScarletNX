/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The FP32 denormal mode a translated shader declares: DenormFlushToZero when every FADD, FMUL and FFMA in it asks
   for a flush (FTZ/FMZ), as the console's compiler emits them, and no mode when one of them keeps denormals or the
   device can't flush. Offline: the translator and glslang, no ICD or GPU. */
#include "gpu/shader/shader.h"
#include "gpu/vk/spirv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCHED     0x001F8000FC0007E0ull /* a control word: no waits */
#define EXIT      0xE30000000007000Full
#define FMUL_FTZ  0x5C68100000070000ull /* FMUL R0, R0, R0, FMZ field (bits 44-45) = FTZ */
#define FMUL_KEEP 0x5C68000000070000ull /* the same with the field clear */
#define FFMA_FMZ  0x59C0000000070000ull /* FFMA R0, R0, R0, R0, FMZ field (bits 53-54) = FMZ */
#define FADD_KEEP 0x5C58000000070000ull /* FADD R0, R0, R0, FTZ (bit 44) clear */

static int failures;
static void check(int ok, const char* what) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

/* a fragment shader: the 80 byte header, then one bundle of a, b and EXIT */
static char* translate(u64 a, u64 b, ShaderInfo* si) {
    static u64 code[64];
    memset(code, 0, sizeof code);
    code[10] = SCHED;
    code[11] = a;
    code[12] = b;
    code[13] = EXIT;
    memset(si, 0, sizeof *si);
    return shader_to_glsl((const u8*)code, sizeof code, ST_FS, si);
}

/* the SPIR-V declares DenormFlushToZero for 32 bit floats: OpCapability 4465 and OpExecutionMode ... 4460 32 */
static int spirv_flushes(const u32* w, size_t n) {
    int cap = 0, mode = 0;
    for (size_t i = 5; i < n;) {
        u32 count = w[i] >> 16, op = w[i] & 0xFFFF;
        if (!count || i + count > n) break;
        if (op == 17 && count >= 2 && w[i + 1] == 4465) cap = 1;
        if (op == 16 && count >= 4 && w[i + 2] == 4460 && w[i + 3] == 32) mode = 1;
        i += count;
    }
    return cap && mode;
}

/* the shader through the renderer's post step and glslang: 1 when it flushes, 0 when not, -1 when it fails */
static int compiled_flushes(u64 a, u64 b, const char* what) {
    ShaderInfo si;
    char* glsl = translate(a, b, &si);
    check(glsl != NULL && !si.unsupported, what);
    if (!glsl) return -1;
    glsl = vk_glsl_post(glsl, ST_FS, shader_flushes_denorms(&si));
    size_t n = 0;
    char err[2048] = "";
    u32* spv = vk_spirv_cached(glsl, ST_FS, NULL, &n, err, sizeof err);
    if (!spv) fprintf(stderr, "%s: %s\n%s\n", what, err, glsl);
    check(spv != NULL, what);
    int flush = spv ? spirv_flushes(spv, n) : -1;
    check((strstr(glsl, "4460, 32") != NULL) == (flush == 1), "the GLSL and the SPIR-V agree");
    free(spv);
    free(glsl);
    return flush;
}

int main(void) {
    ShaderInfo si;
    char* glsl;

    /* what the translator records */
    glsl = translate(FMUL_FTZ, FFMA_FMZ, &si);
    check(si.fp32_flush && !si.fp32_preserve && shader_flushes_denorms(&si), "FTZ and FMZ count as a flush");
    free(glsl);
    glsl = translate(FMUL_FTZ, FADD_KEEP, &si);
    check(si.fp32_flush && si.fp32_preserve && !shader_flushes_denorms(&si), "a FADD without FTZ keeps them");
    free(glsl);
    glsl = translate(FMUL_KEEP, FMUL_KEEP, &si);
    check(!si.fp32_flush && si.fp32_preserve && !shader_flushes_denorms(&si), "FMZ clear keeps them");
    free(glsl);

    /* what the compiled shader declares */
    g_vk_flush_denorm = 1;
    check(compiled_flushes(FMUL_FTZ, FFMA_FMZ, "every op flushes") == 1, "a shader that flushes declares it");
    check(compiled_flushes(FMUL_FTZ, FADD_KEEP, "mixed") == 0, "a mixed shader declares nothing");
    check(compiled_flushes(FMUL_KEEP, FMUL_KEEP, "none flushes") == 0, "a shader that keeps them declares nothing");
    g_vk_flush_denorm = 0;
    check(compiled_flushes(FMUL_FTZ, FFMA_FMZ, "no device support") == 0, "a device that can't flush gets nothing");

    if (failures) return 1;
    puts("FP32 denormal modes: flush declared only for shaders whose every FADD, FMUL and FFMA flushes");
    return 0;
}
