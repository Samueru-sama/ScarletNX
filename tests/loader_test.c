/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "loader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
static char* scratch;
static unsigned char image_bytes[1024], file_bytes[4096], baseline[4096];

static void check(int condition, const char* expression, unsigned line) {
    checks++;
    if (!condition) {
        fprintf(stderr, "loader test line %u: %s (scratch %s)\n", line, expression, scratch ? scratch : "none");
        exit(1);
    }
}
#define CHECK(x) check(!!(x), #x, __LINE__)

static void put32(unsigned char* p, SnxU32 value) {
    unsigned i;
    for (i = 0; i < 4; i++) p[i] = (unsigned char)(value >> (i * 8));
}

static void put64(unsigned char* p, SnxU64 value) {
    put32(p, (SnxU32)value);
    put32(p + 4, (SnxU32)(value >> 32));
}

static void dynamic(unsigned index, unsigned tag, SnxU64 value) {
    put64(image_bytes + 0x100 + index * 16, tag);
    put64(image_bytes + 0x108 + index * 16, value);
}

static void rich_image(void) {
    unsigned i, at;
    memset(image_bytes, 0, sizeof image_bytes);
    put32(image_bytes + 4, 0x80);
    put32(image_bytes + 0x20, 0xd65f03c0UL);
    put32(image_bytes + 0x40, 0xd65f03c0UL);
    put32(image_bytes + 0x50, 0xd65f03c0UL);
    memcpy(image_bytes + 0x80, "MOD0", 4);
    put32(image_bytes + 0x84, 0x100 - 0x80);
    put32(image_bytes + 0x88, 0x400 - 0x80);
    put32(image_bytes + 0x8c, 0x440 - 0x80);
    put32(image_bytes + 0x90, 0x280 - 0x80);
    put32(image_bytes + 0x94, 0x29c - 0x80);
    dynamic(0, 4, 0x220);
    dynamic(1, 5, 0x200);
    dynamic(2, 6, 0x1a0);
    dynamic(3, 10, 0x20);
    dynamic(4, 11, 24);
    dynamic(5, 7, 0x240);
    dynamic(6, 8, 48);
    memcpy(image_bytes + 0x200, "\0old\0entry\0imported\0", 20);
    for (i = 1; i <= 3; i++) {
        at = 0x1a0 + i * 24;
        put32(image_bytes + at, i == 1 ? 1 : i == 2 ? 5 : 11);
        image_bytes[at + 4] = 2;
        image_bytes[at + 6] = i == 3 ? 0 : 1;
        put64(image_bytes + at + 8, i == 3 ? 0 : 0x20);
        put64(image_bytes + at + 16, 0x10);
    }
    put32(image_bytes + 0x220, 1);
    put32(image_bytes + 0x224, 4);
    for (i = 0; i < 2; i++) {
        put64(image_bytes + 0x240 + i * 24, 0x390 + i * 8);
        put64(image_bytes + 0x248 + i * 24, 1027);
        put64(image_bytes + 0x250 + i * 24, 0x30);
    }
    image_bytes[0x280] = 1;
    image_bytes[0x281] = 0x1b;
    image_bytes[0x282] = 3;
    image_bytes[0x283] = 0x3b;
    put32(image_bytes + 0x284, 0x2c0 - 0x284);
    put32(image_bytes + 0x288, 2);
    for (i = 0; i < 2; i++) {
        put32(image_bytes + 0x28c + i * 8, (SnxU32)(0x40 + i * 16) - 0x280);
        put32(image_bytes + 0x290 + i * 8, 0x2e0 + i * 0x20 - 0x280);
        at = 0x2e0 + i * 0x20;
        put32(image_bytes + at, 16);
        put32(image_bytes + at + 4, at + 4 - 0x2c0);
        put32(image_bytes + at + 8, (SnxU32)(0x40 + i * 16) - (at + 8));
        put32(image_bytes + at + 12, 16);
    }
    put32(image_bytes + 0x2c0, 16);
    image_bytes[0x2c8] = 1;
    memcpy(image_bytes + 0x2c9, "zR", 3);
    image_bytes[0x2cc] = 1;
    image_bytes[0x2cd] = 0x78;
    image_bytes[0x2ce] = 30;
    image_bytes[0x2cf] = 1;
    image_bytes[0x2d0] = 0x1b;
}

static size_t literal_block(unsigned char* out, const unsigned char* in, size_t size) {
    size_t used, rest;
    used = 1;
    out[0] = (unsigned char)((size < 15 ? size : 15) << 4);
    if (size >= 15) {
        rest = size - 15;
        while (rest >= 255) {
            out[used++] = 255;
            rest -= 255;
        }
        out[used++] = (unsigned char)rest;
    }
    memcpy(out + used, in, size);
    return used + size;
}

static size_t build_nso(unsigned text_size, unsigned ro_off, unsigned ro_size, unsigned data_off, unsigned data_size,
                        unsigned compressed) {
    unsigned offsets[3], sizes[3], i;
    size_t at, stored;
    memset(file_bytes, 0, sizeof file_bytes);
    memcpy(file_bytes, "NSO0", 4);
    put32(file_bytes + 12, compressed);
    put32(file_bytes + 0x3c, 0x40);
    for (i = 0; i < 32; i++) file_bytes[0x40 + i] = (unsigned char)i;
    offsets[0] = 0;
    offsets[1] = ro_off;
    offsets[2] = data_off;
    sizes[0] = text_size;
    sizes[1] = ro_size;
    sizes[2] = data_size;
    at = 0x100;
    for (i = 0; i < 3; i++) {
        put32(file_bytes + 0x10 + i * 16, (SnxU32)at);
        put32(file_bytes + 0x14 + i * 16, offsets[i]);
        put32(file_bytes + 0x18 + i * 16, sizes[i]);
        if (compressed & (1U << i))
            stored = literal_block(file_bytes + at, image_bytes + offsets[i], sizes[i]);
        else {
            memcpy(file_bytes + at, image_bytes + offsets[i], sizes[i]);
            stored = sizes[i];
        }
        put32(file_bytes + 0x60 + i * 4, (SnxU32)stored);
        at += stored;
    }
    return at;
}

static char* fixture(const char* name, const void* bytes, size_t size) {
    char* path;
    path = snx_path_join(scratch, name);
    CHECK(path != NULL);
    CHECK(!snx_write_file(path, bytes, size));
    return path;
}

static void expect_rich(const SnxModuleImage* m) {
    CHECK(m->image_size == 1024 && !memcmp(m->image, image_bytes, 1024));
    CHECK(m->text_off == 0 && m->text_size == 0x100 && m->ro_off == 0x100 && m->ro_size == 0x280);
    CHECK(m->data_off == 0x380 && m->data_size == 0x80 && m->bss == 0x40 && m->size == 0x1000);
    CHECK(m->build_id[0] == 0 && m->build_id[31] == 31);
    CHECK(m->has_mod0 && m->mod0.dynamic == 0x100 && m->mod0.bss_start == 0x400 && m->mod0.bss_end == 0x440);
    CHECK(m->symbol_count == 1 && m->symbols[0].value == 0x20 && !strcmp(m->symbols[0].name, "entry"));
    CHECK(m->import_count == 1 && !strcmp(m->imports[0], "imported"));
    CHECK(m->function_count == 3 && m->functions[0] == 0x20 && m->functions[1] == 0x40 && m->functions[2] == 0x50);
    CHECK(m->relptr_count == 1 && m->relptrs[0] == 0x30);
    CHECK(m->fde_count == 2 && m->fde[0].start == 0x40 && m->fde[0].size == 16 && m->fde[1].start == 0x50);
    CHECK(m->eh_error == NULL);
}

static void test_nso(void) {
    SnxModuleImage m;
    char error[256], *path;
    size_t size, n;
    unsigned compressed;
    rich_image();
    for (compressed = 0; compressed < 8; compressed++) {
        size = build_nso(0x100, 0x100, 0x280, 0x380, 0x80, compressed);
        path = fixture("fixture.nso", file_bytes, size);
        CHECK(!snx_load_nso(path, "main", &m, error, sizeof error));
        expect_rich(&m);
        snx_free_module(&m);
        free(path);
    }
    size = build_nso(0x100, 0x100, 0x280, 0x380, 0x80, 0);
    memcpy(baseline, file_bytes, size);
    path = snx_path_join(scratch, "fixture.nso");
    for (n = 0; n < size; n++) {
        CHECK(!snx_write_file(path, baseline, n));
        CHECK(snx_load_nso(path, NULL, &m, error, sizeof error) == -1);
        CHECK(m.image == NULL && m.symbol_count == 0 && error[0]);
    }
    memcpy(file_bytes, baseline, size);
    put32(file_bytes + 0x100 + 0x298, 0x7fffffffUL);
    CHECK(!snx_write_file(path, file_bytes, size));
    CHECK(!snx_load_nso(path, "partial", &m, error, sizeof error));
    CHECK(m.eh_error && m.fde_count == 1 && m.function_count == 2 && m.functions[1] == 0x40);
    snx_free_module(&m);
    memcpy(file_bytes, baseline, size);
    put64(file_bytes + 0x100 + 0x148, 0);
    CHECK(!snx_write_file(path, file_bytes, size));
    CHECK(snx_load_nso(path, NULL, &m, error, sizeof error) == -1);
    CHECK(strstr(error, "symbol entry") != NULL && !m.image);
    memcpy(file_bytes, baseline, size);
    put32(file_bytes + 0x100 + 0x1b8, 0xffffffffUL);
    CHECK(!snx_write_file(path, file_bytes, size));
    CHECK(snx_load_nso(path, NULL, &m, error, sizeof error) == -1);
    CHECK(strstr(error, "symbol name") != NULL);
    memset(error, 0x7f, sizeof error);
    CHECK(!snx_write_file(path, "bad", 3));
    CHECK(snx_load_nso(path, NULL, &m, error, 3) == -1);
    CHECK(error[2] == 0 && error[3] == 0x7f);
    free(path);
    puts("PASS NSO segments, LZ4 variants, symbols/imports, relocations, unwind and truncated bounds");
}

static unsigned nibble(char c) { return c <= '9' ? (unsigned)(c - '0') : (unsigned)(c - 'a' + 10); }

static void digest_bytes(unsigned char* out, const char* hex) {
    unsigned i;
    CHECK(strlen(hex) == 64);
    for (i = 0; i < 32; i++) out[i] = (unsigned char)((nibble(hex[i * 2]) << 4) | nibble(hex[i * 2 + 1]));
}

static void test_hash_lz4(void) {
    static const char* messages[3] = {"abc", "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
                                      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"};
    static const char* hashes[3] = {"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
                                    "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
                                    "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb"};
    /* "ABCD", a 4-byte match 4 back, then the 8 literals an LZ4 block has to end with: the last match ends at least 5
       bytes and starts at least 12 bytes before the end of the block, which LZ4_decompress_safe holds a block to */
    static const unsigned char matched[16] = {0x40, 'A', 'B', 'C', 'D', 4,   0,   0x80,
                                              'E',  'F', 'G', 'H', 'I', 'J', 'K', 'L'};
    SnxModuleImage m;
    char error[256], *path;
    size_t size, length;
    unsigned i;
    for (i = 0; i < 3; i++) {
        memset(image_bytes, 0, sizeof image_bytes);
        length = strlen(messages[i]);
        memcpy(image_bytes, messages[i], length);
        size = build_nso((unsigned)length, (unsigned)length, 0, 128, 0, 1);
        put32(file_bytes + 12, 0x19);
        digest_bytes(file_bytes + 0xa0, hashes[i]);
        digest_bytes(file_bytes + 0xc0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
        path = fixture("hash.nso", file_bytes, size);
        CHECK(!snx_load_nso(path, NULL, &m, error, sizeof error));
        CHECK(!memcmp(m.image, messages[i], length));
        snx_free_module(&m);
        file_bytes[0xa0] ^= 1;
        CHECK(!snx_write_file(path, file_bytes, size));
        CHECK(snx_load_nso(path, NULL, &m, error, sizeof error) == -1 && strstr(error, "SHA-256"));
        free(path);
    }
    memset(image_bytes, 0, sizeof image_bytes);
    build_nso(16, 16, 0, 16, 0, 0);
    put32(file_bytes + 12, 1);
    put32(file_bytes + 0x60, sizeof matched);
    put32(file_bytes + 0x20, 0x100 + sizeof matched);
    put32(file_bytes + 0x30, 0x100 + sizeof matched);
    memcpy(file_bytes + 0x100, matched, sizeof matched);
    size = 0x100 + sizeof matched;
    path = fixture("match.nso", file_bytes, size);
    CHECK(!snx_load_nso(path, NULL, &m, error, sizeof error));
    CHECK(!memcmp(m.image, "ABCDABCDEFGHIJKL", 16));
    snx_free_module(&m);
    /* a match reaching before the start of the segment (LZ4 decodes an offset of 0 in bounds, and leaves the
       content to the segment's hash) */
    file_bytes[0x105] = 5;
    CHECK(!snx_write_file(path, file_bytes, size));
    CHECK(snx_load_nso(path, NULL, &m, error, sizeof error) == -1 && strstr(error, "LZ4"));
    file_bytes[0x105] = 4;
    file_bytes[0x100] = 0x4f;
    CHECK(!snx_write_file(path, file_bytes, size));
    CHECK(snx_load_nso(path, NULL, &m, error, sizeof error) == -1 && strstr(error, "LZ4"));
    free(path);
    puts("PASS SHA-256 known answers, corrupt hashes and LZ4 back-reference bounds");
}

static size_t build_nro(void) {
    rich_image();
    memcpy(file_bytes, image_bytes, sizeof image_bytes);
    memcpy(file_bytes + 0x10, "NRO0", 4);
    put32(file_bytes + 0x18, 0x400);
    put32(file_bytes + 0x20, 0);
    put32(file_bytes + 0x24, 0x100);
    put32(file_bytes + 0x28, 0x100);
    put32(file_bytes + 0x2c, 0x280);
    put32(file_bytes + 0x30, 0x380);
    put32(file_bytes + 0x34, 0x80);
    put32(file_bytes + 0x38, 0x40);
    memset(file_bytes + 0x400, 0, 0x40);
    memcpy(file_bytes + 0x400, "ASET", 4);
    put64(file_bytes + 0x428, 0x38);
    put64(file_bytes + 0x430, 5);
    memcpy(file_bytes + 0x438, "ROMFS", 5);
    return 0x43d;
}

static void test_nro_npdm(void) {
    SnxModuleImage m;
    SnxProgram program;
    SnxNpdm npdm;
    unsigned char meta[0xa0];
    char error[256], *path;
    size_t size, i;
    static const unsigned offsets[] = {0x20, 0x24, 0x28, 0x30, 0x34, 0x1c};
    static const unsigned values[] = {4, 0x80, 0x104, 0x384, 0x7c, 2};
    size = build_nro();
    path = fixture("sample.NRO", file_bytes, size);
    CHECK(!snx_load_program(path, &program, error, sizeof error));
    CHECK(program.module_count == 1 && program.modules[0].is_nro && !strcmp(program.modules[0].name, "main"));
    CHECK(program.modules[0].base == SNX_CODE_BASE && program.romfs_size == 5 &&
          !memcmp(program.romfs_data, "ROMFS", 5));
    CHECK(program.npdm.stack == 0x100000 && program.npdm.prio == 44 && program.npdm.is64);
    CHECK(program.npdm.title_id == (((SnxU64)0x01000000UL << 32) | 0x1000));
    snx_free_program(&program);
    /* ASET is optional and never part of the executable mapping. */
    CHECK(!snx_write_file(path, file_bytes, 0x400));
    CHECK(!snx_load_nro(path, &m, error, sizeof error));
    CHECK(!m.nro_asset_size && !m.romfs_size && m.image_size == 0x400 && m.size == 0x1000);
    snx_free_module(&m);
    for (i = 0; i < 0x80; ++i) {
        CHECK(!snx_write_file(path, file_bytes, i));
        CHECK(snx_load_nro(path, &m, error, sizeof error) == -1 && !m.image);
    }
    for (i = 0; i < sizeof offsets / sizeof *offsets; ++i) {
        size = build_nro();
        put32(file_bytes + offsets[i], values[i]);
        CHECK(!snx_write_file(path, file_bytes, size));
        CHECK(snx_load_nro(path, &m, error, sizeof error) == -1 && !m.image && strstr(error, "NRO"));
    }
    size = build_nro();
    put64(file_bytes + 0x430, (SnxU64)-1);
    CHECK(!snx_write_file(path, file_bytes, size));
    CHECK(snx_load_nro(path, &m, error, sizeof error) == -1 && strstr(error, "ASET"));
    size = build_nro();
    put32(file_bytes + 0x18, 0xffffffffUL);
    CHECK(!snx_write_file(path, file_bytes, size));
    CHECK(snx_load_nro(path, &m, error, sizeof error) == -1 && !m.image);
    free(path);
    memset(meta, 0, sizeof meta);
    memcpy(meta, "META", 4);
    meta[12] = 7;
    meta[14] = 37;
    meta[15] = 2;
    put32(meta + 0x1c, 0x200000);
    memcpy(meta + 0x20, "Synthetic Loader", 16);
    put32(meta + 0x70, 0x80);
    put32(meta + 0x74, 0x20);
    memcpy(meta + 0x80, "ACI0", 4);
    put64(meta + 0x90, ((SnxU64)0x12345678UL << 32) | 0x9abcdef0UL);
    CHECK(!snx_parse_npdm(meta, sizeof meta, &npdm, error, sizeof error));
    CHECK(npdm.stack == 0x200000 && npdm.prio == 37 && npdm.core == 2 && npdm.aspace == 3 && npdm.is64);
    CHECK(!strcmp(npdm.name, "Synthetic Loader") && npdm.title_id == (((SnxU64)0x12345678UL << 32) | 0x9abcdef0UL));
    path = fixture("npdm.bin", meta, sizeof meta);
    free(path);
    CHECK(snx_parse_npdm(meta, 0x7f, &npdm, error, sizeof error) == -1 && !npdm.stack);
    put32(meta + 0x70, 0xfffffff0UL);
    CHECK(snx_parse_npdm(meta, sizeof meta, &npdm, error, sizeof error) == -1);
    CHECK(!snx_parse_npdm(NULL, 0, &npdm, error, sizeof error) && !strcmp(npdm.name, "Application"));
    puts("PASS NRO assets/program loading and bounded NPDM metadata");
}

static void test_program(void) {
    static const char* names[4] = {"rtld", "main", "subsdk2", "sdk"};
    SnxProgram program;
    char error[256], *root, *exefs, *romfs, *path, *npdm_path;
    unsigned char* meta;
    size_t size, meta_size;
    unsigned i;
    root = snx_path_join(scratch, "utf8-\xc3\xa9-\xe6\x97\xa5");
    exefs = snx_path_join(root, "ExeFS");
    romfs = snx_path_join(root, "RomFS");
    CHECK(!snx_mkdirs(exefs) && !snx_mkdirs(romfs));
    rich_image();
    size = build_nso(0x100, 0x100, 0x280, 0x380, 0x80, 7);
    for (i = 0; i < 4; i++) {
        path = snx_path_join(exefs, names[i]);
        CHECK(!snx_write_file(path, file_bytes, size));
        free(path);
    }
    npdm_path = snx_path_join(scratch, "npdm.bin");
    meta = snx_read_file(npdm_path, &meta_size);
    CHECK(meta != NULL);
    free(npdm_path);
    path = snx_path_join(exefs, "main.npdm");
    CHECK(!snx_write_file(path, meta, meta_size));
    free(meta);
    free(path);
    CHECK(!snx_load_program(root, &program, error, sizeof error));
    CHECK(program.module_count == 4 && program.npdm.prio == 37 && program.romfs_path != NULL);
    CHECK((strstr(program.exefs_path, "ExeFS") || strstr(program.exefs_path, "exefs")) &&
          strstr(program.romfs_path, "RomFS"));
    for (i = 0; i < 4; i++) {
        CHECK(!strcmp(program.modules[i].name, names[i]));
        CHECK(program.modules[i].base == SNX_CODE_BASE + i * 0x2000);
        expect_rich(&program.modules[i]);
    }
    snx_free_program(&program);
    CHECK(!snx_load_program(exefs, &program, error, sizeof error));
    CHECK(program.module_count == 4 && program.root_path != NULL);
    snx_free_program(&program);
    free(root);
    free(exefs);
    free(romfs);
    puts("PASS UTF-8 game/ExeFS paths, module order/bases and RomFS discovery");
}

static void test_mutations(void) {
    SnxModuleImage module;
    char error[256], *path;
    SnxU32 state;
    size_t size, offset;
    unsigned i, j;
    rich_image();
    size = build_nso(0x100, 0x100, 0x280, 0x380, 0x80, 0);
    memcpy(baseline, file_bytes, size);
    path = snx_path_join(scratch, "mutated.nso");
    state = 0x23498765;
    for (i = 0; i < 512; i++) {
        memcpy(file_bytes, baseline, size);
        for (j = 0; j < 4; j++) {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            offset = 0x100 + (state & 1023);
            file_bytes[offset] ^= (unsigned char)(state >> 16);
        }
        CHECK(!snx_write_file(path, file_bytes, size));
        if (!snx_load_nso(path, NULL, &module, error, sizeof error))
            snx_free_module(&module);
        else
            CHECK(error[0] && module.image == NULL && module.symbols == NULL);
    }
    free(path);
    puts("PASS deterministic malformed MOD0/dynamic/unwind mutations");
}

int main(void) {
    scratch = snx_temp_dir(NULL);
    CHECK(scratch != NULL);
    test_nso();
    test_hash_lz4();
    test_nro_npdm();
    test_program();
    test_mutations();
    CHECK(!snx_remove_tree(scratch));
    free(scratch);
    scratch = NULL;
    printf("PASS %u native loader checks\n", checks);
    return 0;
}
