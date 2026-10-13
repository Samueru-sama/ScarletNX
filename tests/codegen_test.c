/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Synthetic ISA/CFG/module tests. The generated ABI fixture is compiled by the
   supplied host Clang, then checks static and interpreted architectural results. */
#include "a64.h"
#include "codegen.h"
#include "codegen_view.h"
#include "module.h"
#include <stdlib.h>
#include <string.h>

static int failures;

static void check(int condition, const char* what) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

static void put32(unsigned char* image, size_t offset, SnxU32 word) {
    unsigned i;
    for (i = 0; i < 4; ++i) image[offset + i] = (unsigned char)(word >> (8 * i));
}

static void decoder_test(void) {
    static const SnxU32 invalid[] = {0,
                                     0xFFFFFFFFUL,
                                     0x1200FC00UL,
                                     0x52C00000UL,
                                     0x53400000UL,
                                     0x13808000UL,
                                     0xF8600800UL,
                                     0x0A008000UL,
                                     0x0BC00000UL,
                                     0x0B201400UL,
                                     0x1E180000UL};
    static const SnxU32 backwards[] = {0x17FFFFFFUL, 0x97FFFFFFUL, 0x54FFFFE0UL, 0xB4FFFFE0UL, 0x3607FFE0UL};
    size_t i;
    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
        check(snx_a64_decode(invalid[i])->kind == SNX_FLOW_UNDEF, "reject reserved/invalid encoding");
    check(snx_a64_decode(0xD503201FUL)->kind == SNX_FLOW_PLAIN, "NOP decode");
    check(snx_a64_decode(0xD65F03C0UL)->kind == SNX_FLOW_RET, "RET decode");
    check(snx_a64_decode(0x94000002UL)->kind == SNX_FLOW_BL, "BL decode");
    for (i = 0; i < sizeof(backwards) / sizeof(backwards[0]); ++i) {
        check(snx_a64_target(backwards[i], 0x1000) == 0xFFC, "sign-extend negative branch offset");
        check(snx_a64_target(backwards[i], 0) == ~(SnxU64)3, "wrap negative branch target to 64 bits");
    }
}

static int has_leader(const SnxFunction* function, SnxU64 address) {
    size_t i;
    for (i = 0; i < function->count; ++i)
        if (snx_function_address(function, i) == address) return function->leaders[i] != 0;
    return 0;
}

static void setup_program(SnxProgram* program, SnxModuleImage* module, unsigned char* image, size_t size) {
    memset(program, 0, sizeof(*program));
    memset(module, 0, sizeof(*module));
    module->name = "synthetic";
    module->image = image;
    module->image_size = size;
    module->base = SNX_CODE_BASE;
    module->size = 0x1000;
    module->text_size = (SnxU32)size;
    program->modules = module;
    program->module_count = 1;
    program->npdm.title_id = ((SnxU64)0x12345678UL << 32) | 0x9ABCDEF0UL;
    program->npdm.stack = 0x100000;
    program->npdm.prio = 44;
    program->npdm.core = 2;
}

static void analysis_and_module_test(const char* directory) {
    static const SnxU32 words[] = {0xAA1E03F3UL, 0x91000400UL, 0x9400000AUL, 0xF1002C1FUL, 0x540000A1UL,
                                   0x91000800UL, 0xAA1303FEUL, 0xD65F03C0UL, 0xD503201FUL, 0x91019000UL,
                                   0x17FFFFFCUL, 0xD4200000UL, 0x91002800UL, 0xD65F03C0UL};
    unsigned char image[sizeof(words)];
    SnxProgram program;
    SnxModuleImage module;
    SnxCodegen codegen;
    SnxFde ranges[2];
    SnxU64 interior, symbol;
    SnxCodegenOptions options;
    SnxSources sources;
    const SnxFunction* function;
    unsigned char *text, *again;
    size_t i, size, again_size;
    char* saved;
    for (i = 0; i < sizeof(words) / sizeof(words[0]); ++i) put32(image, i * 4, words[i]);
    setup_program(&program, &module, image, sizeof(image));
    ranges[0].start = 0;
    ranges[0].size = 44;
    ranges[1].start = 20;
    ranges[1].size = 8;
    module.fde = ranges;
    module.fde_count = 2;
    symbol = 20;
    module.functions = &symbol;
    module.function_count = 1;
    check(!snx_nx_codegen_analyze(&codegen, &program, NULL, 0, 1), "analyze FDE and BL target");
    check(codegen.function_count == 2, "overlapping FDE discarded, BL function discovered");
    function = snx_codegen_function(&codegen, SNX_CODE_BASE);
    check(function && function->addresses == NULL && function->count == 11, "FDE uses a compact address range");
    if (function) {
        check(!has_leader(function, SNX_CODE_BASE + 20), "discarded FDE start is not an extra symbol seed");
        check(has_leader(function, SNX_CODE_BASE + 12), "BL continuation leader");
        check(has_leader(function, SNX_CODE_BASE + 24), "backwards B target leader");
        check(has_leader(function, SNX_CODE_BASE + 36), "conditional target leader");
    }
    check(snx_codegen_function(&codegen, SNX_CODE_BASE + 48) != NULL, "recursive BL target exists");
    snx_codegen_free(&codegen);
    interior = SNX_CODE_BASE + 20;
    check(!snx_nx_codegen_analyze(&codegen, &program, &interior, 1, 1), "analyze explicit interior hint");
    function = snx_codegen_function(&codegen, SNX_CODE_BASE);
    check(function && has_leader(function, interior), "execution hint adds interior dispatch entry");
    snx_codegen_free(&codegen);

    memset(&options, 0, sizeof(options));
    options.outdir = directory;
    options.title = "Quoted \"title\" \\ UTF-8 \303\251";
    options.chunk_instrs = 4;
    options.quiet = 1;
    options.hints = &interior;
    options.hint_count = 1;
    options.abi = 3;
    check(!snx_generate_module(&program, &options, &sources), "emit chunked module");
    if (sources.count) {
        int direct = 0;
        check(sources.count == 3, "instruction budget splits at function boundaries");
        for (i = 1; i < sources.count; ++i) {
            text = snx_read_file(sources.paths[i], &size);
            if (text && strstr((char*)text, "F_8000030(c,134217776);")) direct = 1;
            free(text);
        }
        check(direct, "default direct BL target stays a static C call");
        text = snx_read_file(sources.paths[0], &size);
        check(text && strstr((char*)text, "MOD.abi=3;") && strstr((char*)text, "MOD.nentries=7;"),
              "module ABI/entry metadata");
        check(text && strstr((char*)text, "MOD.title_id=0x123456789ABCDEF0ULL"), "64-bit NPDM title ID");
        check(text && strstr((char*)text, "\\\"title\\\"") && strstr((char*)text, "\\303\\251"),
              "C string escaping preserves title bytes");
        saved = snx_strdup(sources.paths[0]);
        snx_sources_free(&sources);
        check(!snx_generate_module(&program, &options, &sources), "regenerate unchanged module");
        again = snx_read_file(saved, &again_size);
        check(text && again && size == again_size && !memcmp(text, again, size), "deterministic source cache content");
        free(text);
        free(again);
        free(saved);
    }
    snx_sources_free(&sources);
    options.hook_direct_calls = 1;
    check(!snx_generate_module(&program, &options, &sources), "emit direct-call hook module");
    {
        int routed = 0;
        for (i = 1; i < sources.count; ++i) {
            text = snx_read_file(sources.paths[i], &size);
            if (text && strstr((char*)text, "CALLDYN(134217776);")) routed = 1;
            free(text);
        }
        check(routed, "direct BL target uses dispatch in hook mode");
    }
    snx_sources_free(&sources);
    options.hook_direct_calls = 0;
    options.interp_only = 1;
    check(!snx_generate_module(&program, &options, &sources), "emit interpreter-only module");
    if (sources.count) {
        check(sources.count == 1, "interpreter-only has no AOT chunks");
        text = snx_read_file(sources.paths[0], &size);
        check(text && strstr((char*)text, "MOD.nentries=0;"), "interpreter-only empty entry table");
        free(text);
    }
    snx_sources_free(&sources);
}

static void large_range_test(void) {
    SnxProgram program;
    SnxModuleImage module;
    SnxCodegen codegen;
    SnxFde range;
    unsigned char* image;
    size_t i, size;
    size = 1024 * 1024;
    image = (unsigned char*)snx_alloc(size);
    for (i = 0; i < size; i += 4) put32(image, i, 0xD503201FUL);
    put32(image, size - 4, 0xD65F03C0UL);
    setup_program(&program, &module, image, size);
    range.start = 0;
    range.size = size;
    module.fde = &range;
    module.fde_count = 1;
    check(!snx_nx_codegen_analyze(&codegen, &program, NULL, 0, 1), "analyze a large FDE");
    check(codegen.function_count == 1 && codegen.functions[0].addresses == NULL &&
              codegen.functions[0].count == size / 4 && codegen.functions[0].leader_count == 1,
          "large FDE stays a range, without one address allocation per instruction");
    snx_codegen_free(&codegen);
    free(image);
}

static void emit_semantic_fixture(FILE* out) {
    static const SnxU32 words[] = {
        0x31000420UL, /* ADDS W0,W1,#1: zero-extension and NZCV. */
        0xBA1F03E0UL, /* ADCS X0,XZR,XZR: integer carry input. */
        0xDA1F03E0UL, /* SBC X0,XZR,XZR: 64-bit complement of zero. */
        0xD3485C20UL, /* UBFX X0,X1,#8,#16. */
        0x93401C20UL, /* SXTB X0,W1. */
        0x93C12020UL, /* ROR X0,X1,#8. */
        0x9E220020UL, /* SCVTF S0,X1: direct int64 -> float32 rounding. */
        0x9E660020UL, /* FMOV X0,D1: preserve NaN payload bits. */
        0x1E620C20UL, /* FCSEL D0,D1,D2,EQ: preserve selected bits. */
        0x4EA28420UL, /* ADD V0.4S,V1.4S,V2.4S. */
        0x4E22CC20UL, /* FMLA V0.4S,V1.4S,V2.4S. */
        0x4E040C20UL, /* DUP V0.4S,W1. */
        0x4E183C20UL, /* UMOV X0,V1.D[1]. */
        0x5FA21820UL, /* FMLA S0,S1,V2.S[3]: scalar upper-lane clearing. */
        0xF94007E0UL, /* LDR X0,[SP,#8]. */
        0xF900043FUL, /* STR XZR,[X1,#8]. */
        0x17FFFFFFUL, /* B pc-4. */
        0x1200FC00UL  /* Invalid logical immediate traps in both paths. */
    };
    size_t i;
    char function[32];
    fputs("#include \"cpu.h\"\n#include \"cpu_helpers.c\"\n#include <stdio.h>\nGuestFn snx_interp_entry;\n", out);
    for (i = 0; i < sizeof(words) / sizeof(words[0]); ++i) {
        sprintf(function, "step_%lu", (unsigned long)i);
        snx_a64_static_step(out, words[i], 0x1000, function);
    }
    snx_a64_interp(out, 1);
    fputs("typedef u64(*Step)(CPU*);\nstatic const Step steps[]={", out);
    for (i = 0; i < sizeof(words) / sizeof(words[0]); ++i) fprintf(out, "%sstep_%lu", i ? "," : "", (unsigned long)i);
    fputs("};\nstatic const u32 words[]={", out);
    for (i = 0; i < sizeof(words) / sizeof(words[0]); ++i)
        fprintf(out, "%s0x%08lXu", i ? "," : "", (unsigned long)words[i]);
    fputs("};\nstatic int traps;\nstatic void undefined(CPU*c,u64 pc,u32 w){(void)c;(void)pc;(void)w;++traps;}\n", out);
    fputs("int main(void){ unsigned mode,i,j; CPU c; RtAPI rt; u64 pc,value; u8 memory[64]; int ok;\n", out);
    fputs(" memset(&rt,0,sizeof(rt));rt.undef=undefined;\n for(mode=0;mode<2;mode++)for(i=0;i<18;i++){\n", out);
    fputs(" memset(&c,0,sizeof(c));memset(memory,0xAA,sizeof(memory));c.mem=memory;c.rt=&rt;traps=0;\n switch(i){\n",
          out);
    fputs(" case 0:c.x[1]=0xFFFFFFFFULL;break; case 1:c.cf=1;break; case 2:c.cf=0;break;\n", out);
    fputs(" case 3:c.x[1]=0x12345678;break;case 4:c.x[1]=0x80;break;case 5:c.x[1]=0x123456789ABCDEF0ULL;break;\n", out);
    fputs(" case 6:c.x[1]=16777217;break;case 7:c.v[1].d[0]=0x7FF8000000001234ULL;break;\n", out);
    fputs(" case 8:c.z=1;c.v[1].d[0]=0x7FF8000000001234ULL;c.v[2].d[0]=0x3FF0000000000000ULL;break;\n", out);
    fputs(" case 9:for(j=0;j<4;j++){c.v[1].s[j]=0xFFFFFFFFu;c.v[2].s[j]=j+1;}break;\n", out);
    fputs(" case 10:for(j=0;j<4;j++){c.v[0].f[j]=3;c.v[1].f[j]=4;c.v[2].f[j]=5;}break;\n", out);
    fputs(" case 11:c.x[1]=0x123456789ABCDEF0ULL;break;case 12:c.v[1].d[1]=0xFEDCBA9876543210ULL;break;\n", out);
    fputs(" case 13:c.v[0].d[1]=~0ULL;c.v[0].f[0]=3;c.v[1].f[0]=4;c.v[2].f[3]=5;break;\n", out);
    fputs(" case 14:c.x[31]=16;value=0x123456789ABCDEF0ULL;memcpy(memory+24,&value,8);break;\n", out);
    fputs(" case 15:c.x[1]=16;c.x[31]=0xFFFFFFFF;break;default:break;}\n", out);
    fputs(" pc=mode?snx_step(&c,words[i],0x1000):steps[i](&c);ok=pc==(i==16?0xFFC:i==17?0x1000:0x1004);\n switch(i){\n",
          out);
    fputs(" case 0:ok&=c.x[0]==0&&c.z==1&&c.cf==1&&!c.n&&!c.vf;break;case 1:ok&=c.x[0]==1;break;\n", out);
    fputs(" case 2:ok&=c.x[0]==~0ULL;break;case 3:ok&=c.x[0]==0x3456;break;case 4:ok&=c.x[0]==~127ULL;break;\n", out);
    fputs(" case 5:ok&=c.x[0]==0xF0123456789ABCDEULL;break;case 6:ok&=c.v[0].s[0]==0x4B800000u&&!c.v[0].d[1];break;\n",
          out);
    fputs(
        " case 7:ok&=c.x[0]==0x7FF8000000001234ULL;break;case 8:ok&=c.v[0].d[0]==0x7FF8000000001234ULL&&!c.v[0].d[1];break;\n",
        out);
    fputs(" case 9:for(j=0;j<4;j++)ok&=c.v[0].s[j]==j;break;case 10:for(j=0;j<4;j++)ok&=c.v[0].f[j]==23;break;\n", out);
    fputs(
        " case 11:for(j=0;j<4;j++)ok&=c.v[0].s[j]==0x9ABCDEF0u;break;case 12:ok&=c.x[0]==0xFEDCBA9876543210ULL;break;\n",
        out);
    fputs(
        " case 13:ok&=c.v[0].f[0]==23&&!c.v[0].s[1]&&!c.v[0].d[1];break;case 14:ok&=c.x[0]==0x123456789ABCDEF0ULL;break;\n",
        out);
    fputs(
        " case 15:memcpy(&value,memory+24,8);ok&=value==0&&c.x[31]==0xFFFFFFFF;break;case 17:ok&=traps==1;break;default:break;}\n",
        out);
    fputs(" if(!ok||(i!=17&&traps)){fprintf(stderr,\"semantic case %u mode %u failed\\n\",i,mode);return 1;}}\n", out);
    fputs(" puts(\"Static/interpreted scalar, FP, SIMD, memory and trap semantics passed\");return 0;}\n", out);
}

static void semantics_test(const char* compiler, const char* repo, const char* directory) {
    const char* args[16];
    char *source, *binary, *include, *output;
    FILE* file;
    int n, status;
    source = snx_path_join(directory, "semantics.c");
#ifdef _WIN32
    binary = snx_path_join(directory, "semantics.exe");
#else
    binary = snx_path_join(directory, "semantics");
#endif
    include = snx_path_join(repo, "src/common/cpu/aarch64");
    output = NULL;
    file = snx_fopen(source, "wb");
    check(file != NULL, "open ISA semantics fixture");
    if (!file) goto done;
    emit_semantic_fixture(file);
    check(!fclose(file), "write ISA semantics fixture");
    n = 0;
    args[n++] = compiler;
    args[n++] = "-O2";
    args[n++] = "-fno-strict-aliasing";
    args[n++] = "-fwrapv";
#if defined(__x86_64__) || defined(_M_X64)
    args[n++] = "-mcx16";
#endif
    args[n++] = "-Wno-unused-label";
    args[n++] = "-I";
    args[n++] = include;
    args[n++] = source;
#ifndef _WIN32
    args[n++] = "-lm";
#endif
    args[n++] = "-o";
    args[n++] = binary;
    args[n] = NULL;
    status = snx_run(args, directory, NULL, &output);
    if (status && output) fputs(output, stderr);
    check(!status, "compile emitted ISA semantics against the runtime ABI");
    free(output);
    output = NULL;
    if (!status) {
        args[0] = binary;
        args[1] = NULL;
        status = snx_run(args, directory, NULL, &output);
        if (output) fputs(output, status ? stderr : stdout);
        check(!status, "execute static/interpreted architectural expectations");
    }
done:
    free(source);
    free(binary);
    free(include);
    free(output);
}

int main(int argc, char** argv) {
    char* directory;
    if (argc != 3) return 2;
    directory = snx_temp_dir(NULL);
    check(directory != NULL, "create codegen test directory");
    if (!directory) return 1;
    decoder_test();
    analysis_and_module_test(directory);
    large_range_test();
    semantics_test(argv[1], argv[2], directory);
    check(!snx_remove_tree(directory), "remove codegen test directory");
    free(directory);
    printf("Native codegen: %s\n", failures ? "FAILED" : "passed");
    return failures != 0;
}
