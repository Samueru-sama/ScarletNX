/* SPDX-License-Identifier: GPL-3.0-or-later */
/* SPDX-FileCopyrightText: Copyright 2026 ScarletNX contributors */
/* Calls the production decoder and instruction emitter, not a transcription. */
#include "gpu/shader/translator.h"

int main(int argc, char** argv) {
    ops_init();
    if (argc == 2 && !strcmp(argv[1], "--merge-test")) {
        unsigned checks = 0, failed = 0;
        for (u32 cond = 0; cond < 32; ++cond)
            for (u32 pred = 0; pred < 16; ++pred) {
                u64 a[32] = {0}, b[32] = {0}, out[32];
                a[11] = 0xE300000000000000ull | ((u64)pred << 16) | cond;
                a[12] = b[11] = 0xE30000000007000Full;
                const Op* before = decode(a[11]);
                if (!before || before->id != O_EXIT) continue;
                ++checks;
                if (!shader_merge_dual((u8*)a, (u8*)b, (u8*)out, sizeof(out)) || decode(out[11])->id != O_BRA ||
                    (out[11] & 31) != cond || B(out[11], 16, 4) != pred)
                    ++failed;
            }
        printf("dual-vertex merge: %u checks, %u failures\n", checks, failed);
        return failed != 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--shader")) {
        FILE* f = fopen(argv[2], "rb");
        if (!f) return 2;
        if (fseek(f, 0, SEEK_END)) return 2;
        long n = ftell(f);
        if (n < 88 || n > 1024 * 1024 || fseek(f, 0, SEEK_SET)) return 2;
        u8* code = malloc((size_t)n);
        if (!code || fread(code, 1, (size_t)n, f) != (size_t)n) return 2;
        fclose(f);
        ShaderInfo si = {.cs_local = {1, 1, 1}};
        char* glsl = shader_to_glsl(code, (u32)n, ST_CS, &si);
        free(code);
        if (!glsl || si.unsupported) return 3;
        fputs(glsl, stdout);
        free(glsl);
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--decode-all")) {
        for (u32 i = 0; i < 65536; ++i) {
            const Op* op = decode((u64)i << 48);
            printf("%s\n", op ? op->name : "UNKNOWN");
        }
        return 0;
    }
    for (int i = 1; i < argc; ++i) {
        char* end;
        u64 word = strtoull(argv[i], &end, 16);
        if (*end) return 2;
        const Op* op = decode(word);
        if (!op) return 3;
        ShaderInfo si = {0};
        Ctx c = {.si = &si, .stage = ST_CS};
        emit_tracked(&c, 11, word, op);
        printf("// %s\n%s\n", op->name, c.out.p ? c.out.p : "");
        free(c.out.p);
        free(c.decl.p);
    }
    return 0;
}
