/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The storage layers cmake/hactool-libhac.patch ports from LibHac into hactool, on synthetic archives encrypted with
   synthetic keys: compressed and sparse sections, an update with AesCtrEx runs (one of them plaintext) and a
   relocation table of several entry sets applied to a sparse base, an update with no AesCtrEx entries, a bucket tree
   with an L2 level, and a gamecard that starts with its key area. With the hactool executable as argument, the update
   is also extracted through the command line, as scarletnx --extract runs it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <direct.h>
#include <process.h>
#else
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#if defined(_WIN32) && defined(__clang__)
/* The pinned upstream header redeclares the MinGW CRT's imported fseeko64. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winconsistent-dllimport"
#endif
#include "nca.h"
#include "xci.h"
#include "sha.h"
#include "storage.h"
#include "lz4.h"
#if defined(_WIN32) && defined(__clang__)
#pragma clang diagnostic pop
#endif

#define IVFC_BLOCK 0x200
#define IVFC_BLOCK_LOG2 9
#define CHUNK 0x40 /* the relocation tables' granularity */

static unsigned checks;

static void check(int good, const char* expression, int line) {
    checks++;
    if (!good) {
        fprintf(stderr, "hactool_layers.c:%d: check failed: %s\n", line, expression);
        exit(90);
    }
}
#define CHECK(value) check((value) != 0, #value, __LINE__)

typedef struct {
    unsigned char* data;
    size_t size;
} blob_t;

static void* zalloc(size_t size) {
    void* p = calloc(1, size ? size : 1);
    CHECK(p != NULL);
    return p;
}

static size_t align_up(size_t value, size_t alignment) { return (value + alignment - 1) / alignment * alignment; }

static void put32(unsigned char* p, uint32_t v) {
    for (unsigned i = 0; i < 4; i++) p[i] = (unsigned char)(v >> (8 * i));
}

static void put64(unsigned char* p, uint64_t v) {
    for (unsigned i = 0; i < 8; i++) p[i] = (unsigned char)(v >> (8 * i));
}

static uint64_t get_le(const unsigned char* p, unsigned size) {
    uint64_t v = 0;
    for (unsigned i = 0; i < size; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

/* Bytes that compress badly, so LZ4 blocks and relocations have something to move. */
static void noise(unsigned char* p, size_t size, uint32_t seed) {
    for (size_t i = 0; i < size; i++) {
        seed = seed * 1103515245U + 12345U;
        p[i] = (unsigned char)(seed >> 16);
    }
}

/* Synthetic keys, never console ones. */
static unsigned char header_key[0x20], key_area_key[0x10], section_keys[0x40];

static void synthetic_keys(hactool_ctx_t* tool) {
    for (unsigned i = 0; i < sizeof(header_key); i++) header_key[i] = (unsigned char)(0x11 + i);
    for (unsigned i = 0; i < sizeof(key_area_key); i++) key_area_key[i] = (unsigned char)(0xA0 + i);
    for (unsigned i = 0; i < sizeof(section_keys); i++) section_keys[i] = (unsigned char)(0x31 + 3 * i);
    memcpy(tool->settings.keyset.header_key, header_key, sizeof(header_key));
    memcpy(tool->settings.keyset.key_area_keys[0][0], key_area_key, sizeof(key_area_key));
}

/* The section counter's upper half, as nca_fs_header_t stores it (little-endian) and as it is used (big-endian). */
static void upper_iv(unsigned char out[8], const nca_fs_header_t* fs, uint32_t generation, int replace) {
    for (unsigned j = 0; j < 8; j++) out[j] = fs->section_ctr[7 - j];
    if (replace) {
        out[4] = (unsigned char)(generation >> 24);
        out[5] = (unsigned char)(generation >> 16);
        out[6] = (unsigned char)(generation >> 8);
        out[7] = (unsigned char)generation;
    }
}

static void ctr_crypt(const unsigned char iv_high[8], uint64_t counter_offset, unsigned char* data, size_t size) {
    unsigned char iv[16];
    uint64_t block = counter_offset / 16;
    CHECK(counter_offset % 16 == 0);
    memcpy(iv, iv_high, 8);
    for (unsigned i = 0; i < 8; i++) iv[15 - i] = (unsigned char)(block >> (8 * i));
    aes_ctx_t* aes = new_aes_ctx(section_keys + 0x20, 16, AES_MODE_CTR);
    aes_setiv(aes, iv, sizeof(iv));
    aes_encrypt(aes, data, data, size);
    free_aes_ctx(aes);
}

/* A bucket tree as the official builder writes one: an L1 node, any L2 nodes, then entry sets of one node each. */
static blob_t tree_build(const unsigned char* entries, size_t entry_size, uint32_t count, uint64_t end) {
    uint32_t per_set = (uint32_t)((BUCKET_TREE_NODE_SIZE - 0x10) / entry_size);
    uint32_t sets = (count + per_set - 1) / per_set;
    uint32_t offsets = (BUCKET_TREE_NODE_SIZE - 0x10) / 8;
    uint64_t node_size = bucket_tree_query_node_storage_size(entry_size, count);
    blob_t tree = {0};
    tree.size = (size_t)(node_size + (uint64_t)sets * BUCKET_TREE_NODE_SIZE);
    tree.data = zalloc(tree.size);
    put32(tree.data + 4, sets < offsets ? sets : offsets);
    put64(tree.data + 8, end);
    for (uint32_t i = 0; i < sets && i < offsets; i++)
        memcpy(tree.data + 0x10 + 8 * i, entries + (size_t)i * per_set * entry_size, 8);
    for (uint32_t n = 1; n < node_size / BUCKET_TREE_NODE_SIZE; n++) {
        put32(tree.data + n * BUCKET_TREE_NODE_SIZE, n);
        put32(tree.data + n * BUCKET_TREE_NODE_SIZE + 4, 1);
        put64(tree.data + n * BUCKET_TREE_NODE_SIZE + 8, end);
    }
    for (uint32_t i = 0; i < sets; i++) {
        unsigned char* set = tree.data + node_size + (size_t)i * BUCKET_TREE_NODE_SIZE;
        uint32_t first = i * per_set;
        uint32_t n = count - first < per_set ? count - first : per_set;
        put32(set, i);
        put32(set + 4, n);
        if (first + n < count)
            memcpy(set + 8, entries + (size_t)(first + n) * entry_size, 8);
        else
            put64(set + 8, end);
        memcpy(set + 0x10, entries + (size_t)first * entry_size, (size_t)n * entry_size);
    }
    return tree;
}

static void tree_header(bucket_tree_header_t* header, uint32_t count) {
    header->magic = BUCKET_TREE_MAGIC;
    header->version = 1;
    header->entry_count = (int32_t)count;
}

static void indirect_entry(unsigned char* entry, uint64_t virtual_offset, uint64_t physical_offset, uint32_t storage) {
    put64(entry, virtual_offset);
    put64(entry + 8, physical_offset);
    put32(entry + 0x10, storage);
}

/* A RomFS of a root directory and its files. */
typedef struct {
    const char* name;
    blob_t content;
} romfs_file_t;

static blob_t romfs_build(const romfs_file_t* files, unsigned count) {
    size_t meta = 0;
    for (unsigned i = 0; i < count; i++) meta += 0x20 + align_up(strlen(files[i].name), 4);
    size_t data_offset = align_up(0x70 + meta, 0x10);
    size_t size = data_offset;
    for (unsigned i = 0; i < count; i++) size = align_up(size, 0x10) + files[i].content.size;
    blob_t romfs = {zalloc(size), size};
    unsigned char* h = romfs.data;
    put64(h, ROMFS_HEADER_SIZE);
    put64(h + 0x08, 0x50); /* directory hashes */
    put64(h + 0x10, 4);
    put64(h + 0x18, 0x54); /* directories */
    put64(h + 0x20, 0x18);
    put64(h + 0x28, 0x6C); /* file hashes */
    put64(h + 0x30, 4);
    put64(h + 0x38, 0x70); /* files */
    put64(h + 0x40, meta);
    put64(h + 0x48, data_offset);
    put32(h + 0x50, 0);
    put32(h + 0x54 + 0x4, ROMFS_ENTRY_EMPTY); /* root: no sibling, no child, files from 0 */
    put32(h + 0x54 + 0x8, ROMFS_ENTRY_EMPTY);
    put32(h + 0x54 + 0x10, ROMFS_ENTRY_EMPTY);
    put32(h + 0x6C, 0);
    size_t entry = 0, data = 0;
    for (unsigned i = 0; i < count; i++) {
        unsigned char* f = h + 0x70 + entry;
        size_t name = strlen(files[i].name);
        size_t next = entry + 0x20 + align_up(name, 4);
        data = align_up(data, 0x10);
        put32(f + 0x4, i + 1 < count ? (uint32_t)next : ROMFS_ENTRY_EMPTY);
        put64(f + 0x8, data);
        put64(f + 0x10, files[i].content.size);
        put32(f + 0x18, ROMFS_ENTRY_EMPTY);
        put32(f + 0x1C, (uint32_t)name);
        memcpy(f + 0x20, files[i].name, name);
        memcpy(h + data_offset + data, files[i].content.data, files[i].content.size);
        data += files[i].content.size;
        entry = next;
    }
    return romfs;
}

/* image split into 0x1000-byte blocks stored in LZ4, as zeros (blocks of zeros) or as is (every fifth), followed by
   the compression table. */
static blob_t compress_build(const blob_t* image, nca_compression_info_t* info) {
    uint32_t count = (uint32_t)((image->size + 0xFFF) / 0x1000);
    unsigned char* entries = zalloc((size_t)count * COMPRESSED_ENTRY_SIZE);
    unsigned char* physical = zalloc(image->size + count * 0x100 + 0x10);
    unsigned char zeros[0x1000] = {0};
    size_t used = 0;
    for (uint32_t i = 0; i < count; i++) {
        size_t offset = (size_t)i * 0x1000;
        size_t size = image->size - offset < 0x1000 ? image->size - offset : 0x1000;
        unsigned char* entry = entries + (size_t)i * COMPRESSED_ENTRY_SIZE;
        put64(entry, offset);
        if (size == 0x1000 && !memcmp(image->data + offset, zeros, size)) {
            entry[0x10] = COMPRESSION_ZEROED;
            continue;
        }
        put64(entry + 8, used);
        if (i % 5 == 4) {
            entry[0x10] = COMPRESSION_NONE;
            memcpy(physical + used, image->data + offset, size);
            put32(entry + 0x14, (uint32_t)size);
            used += size;
        } else {
            int n = LZ4_compress_default((const char*)image->data + offset, (char*)physical + used, (int)size,
                                         (int)(image->size + count * 0x100 - used));
            CHECK(n > 0);
            entry[0x10] = COMPRESSION_LZ4;
            put32(entry + 0x14, (uint32_t)n);
            used += (size_t)n;
        }
    }
    blob_t table = tree_build(entries, COMPRESSED_ENTRY_SIZE, count, image->size);
    size_t table_offset = align_up(used, 0x10);
    blob_t out = {zalloc(table_offset + table.size), table_offset + table.size};
    memcpy(out.data, physical, used);
    memcpy(out.data + table_offset, table.data, table.size);
    info->table_offset = table_offset;
    info->table_size = table.size;
    tree_header(&info->table_header, count);
    free(entries);
    free(physical);
    free(table.data);
    return out;
}

/* IVFC levels 0 to 4 hashing data (level 5), from offset 0 of a new section image. */
static blob_t ivfc_build(const blob_t* data, ivfc_hdr_t* hdr) {
    uint64_t size[IVFC_MAX_LEVEL], offset[IVFC_MAX_LEVEL];
    size[IVFC_MAX_LEVEL - 1] = data->size;
    for (int i = IVFC_MAX_LEVEL - 2; i >= 0; i--) size[i] = (size[i + 1] + IVFC_BLOCK - 1) / IVFC_BLOCK * 0x20;
    CHECK(size[0] <= IVFC_BLOCK);
    offset[0] = 0;
    for (unsigned i = 1; i < IVFC_MAX_LEVEL; i++) offset[i] = align_up(offset[i - 1] + size[i - 1], IVFC_BLOCK);
    blob_t section = {0};
    section.size = align_up(offset[IVFC_MAX_LEVEL - 1] + size[IVFC_MAX_LEVEL - 1], 0x200);
    section.data = zalloc(section.size + IVFC_BLOCK);
    memcpy(section.data + offset[IVFC_MAX_LEVEL - 1], data->data, data->size);
    for (int i = IVFC_MAX_LEVEL - 2; i >= 0; i--) {
        for (uint64_t b = 0; b * IVFC_BLOCK < size[i + 1]; b++) {
            /* A short last block is hashed padded with zeros (hactool's full_block). */
            unsigned char block[IVFC_BLOCK] = {0};
            uint64_t n = size[i + 1] - b * IVFC_BLOCK < IVFC_BLOCK ? size[i + 1] - b * IVFC_BLOCK : IVFC_BLOCK;
            memcpy(block, section.data + offset[i + 1] + b * IVFC_BLOCK, n);
            sha256_hash_buffer(section.data + offset[i] + b * 0x20, block, IVFC_BLOCK);
        }
    }
    memset(hdr, 0, sizeof(*hdr));
    hdr->magic = MAGIC_IVFC;
    hdr->id = 0x20000;
    hdr->master_hash_size = 0x20;
    hdr->num_levels = IVFC_MAX_LEVEL + 1;
    for (unsigned i = 0; i < IVFC_MAX_LEVEL; i++) {
        hdr->level_headers[i].logical_offset = offset[i];
        hdr->level_headers[i].hash_data_size = size[i];
        hdr->level_headers[i].block_size = IVFC_BLOCK_LOG2;
    }
    unsigned char block[IVFC_BLOCK] = {0};
    memcpy(block, section.data, size[0]);
    sha256_hash_buffer(hdr->master_hash, block, IVFC_BLOCK);
    return section;
}

/* An NCA3 of program content: the header in XTS, its key area encrypted with the synthetic key-area key. body holds
   the sections at their offsets from 0xC00. */
static blob_t nca_build(nca_header_t* header, const blob_t* body) {
    header->magic = MAGIC_NCA3;
    header->content_type = NCACONTENTTYPE_PROGRAM;
    header->fixed_key_generation = 0xFF;
    header->title_id = 0x0100000000AB0000ULL;
    header->nca_size = 0xC00 + body->size;
    aes_ctx_t* aes = new_aes_ctx(key_area_key, 16, AES_MODE_ECB);
    aes_encrypt(aes, header->encrypted_keys, section_keys, sizeof(section_keys));
    free_aes_ctx(aes);
    blob_t nca = {zalloc(0xC00 + body->size), 0xC00 + body->size};
    aes = new_aes_ctx(header_key, 32, AES_MODE_XTS);
    aes_xts_encrypt(aes, nca.data, header, 0xC00, 0, 0x200);
    free_aes_ctx(aes);
    memcpy(nca.data + 0xC00, body->data, body->size);
    return nca;
}

static void section_entry(nca_header_t* header, unsigned i, size_t start, size_t size) {
    CHECK(start % 0x200 == 0 && size % 0x200 == 0);
    header->section_entries[i].media_start_offset = (uint32_t)(start / 0x200);
    header->section_entries[i].media_end_offset = (uint32_t)((start + size) / 0x200);
}

static FILE* archive(const blob_t* blob) {
    FILE* file = tmpfile();
    CHECK(file != NULL);
    CHECK(fwrite(blob->data, 1, blob->size, file) == blob->size);
    CHECK(!fflush(file));
    rewind(file);
    return file;
}

static void process(nca_ctx_t* ctx, hactool_ctx_t* tool, FILE* file) {
    nca_init(ctx);
    ctx->tool_ctx = tool;
    ctx->file = file;
    nca_process(ctx);
}

/* Every file of a RomFS section reads back as expected, and its tables are what hactool loaded. */
static void romfs_matches(nca_section_ctx_t* section, romfs_ctx_t* romfs_view, bktr_section_ctx_t* bktr_view,
                          const romfs_file_t* files, unsigned count) {
    romfs_fentry_t* table = romfs_view ? romfs_view->files : bktr_view->files;
    romfs_hdr_t* header = romfs_view ? &romfs_view->header : &bktr_view->header;
    uint64_t romfs_offset = romfs_view ? romfs_view->romfs_offset : bktr_view->romfs_offset;
    CHECK(table != NULL);
    CHECK(header->header_size == ROMFS_HEADER_SIZE);
    uint32_t offset = 0;
    for (unsigned i = 0; i < count; i++) {
        /* Entries are 4-aligned, romfs_fentry_t 8-aligned: read its fields as bytes. */
        const unsigned char* entry = (const unsigned char*)table + offset;
        size_t name = strlen(files[i].name);
        CHECK(get_le(entry + 0x1C, 4) == name && !memcmp(entry + 0x20, files[i].name, name));
        CHECK(get_le(entry + 0x10, 8) == files[i].content.size);
        unsigned char* got = zalloc(files[i].content.size);
        nca_section_fseek(section, romfs_offset + header->data_offset + get_le(entry + 8, 8));
        CHECK(nca_section_fread(section, got, files[i].content.size) == files[i].content.size);
        CHECK(!memcmp(got, files[i].content.data, files[i].content.size));
        free(got);
        offset = (uint32_t)get_le(entry + 4, 4);
    }
    CHECK(offset == ROMFS_ENTRY_EMPTY);
}

static void levels_valid(const ivfc_level_ctx_t* levels) {
    for (unsigned i = 0; i < IVFC_MAX_LEVEL; i++) CHECK(levels[i].hash_validity == VALIDITY_VALID);
}

/* Files of every kind of block: compressible, zeros, noise, and a short tail. hactool prints a RomFS name with %s,
   reading past one whose length is a multiple of 4 (no NUL in the table), so none is. */
static romfs_file_t files_a[3], files_b[4];

static void make_files(void) {
    files_a[0].name = "texts.bin";
    files_a[0].content.size = 0x5321;
    files_a[0].content.data = zalloc(0x5321);
    for (size_t i = 0; i < 0x5321; i++) files_a[0].content.data[i] = (unsigned char)"ScarletNX "[i % 10];
    files_a[1].name = "zeros.bin";
    files_a[1].content.size = 0x3000;
    files_a[1].content.data = zalloc(0x3000);
    files_a[2].name = "noise.bin";
    files_a[2].content.size = 0x4010;
    files_a[2].content.data = zalloc(0x4010);
    noise(files_a[2].content.data, 0x4010, 7);
    files_b[0] = files_a[0];
    files_b[0].name = "copies.bin";
    files_b[1].name = "noise.bin";
    files_b[1].content.size = 0x21345;
    files_b[1].content.data = zalloc(0x21345);
    noise(files_b[1].content.data, 0x21345, 99);
    files_b[2] = files_a[1];
    files_b[3].name = "tails.bin";
    files_b[3].content.size = 0x31;
    files_b[3].content.data = zalloc(0x31);
    noise(files_b[3].content.data, 0x31, 5);
}

/* A base game's RomFS in LZ4 blocks: hashes over the stored blocks, files from the decompressed ones. */
static void compressed_romfs(void) {
    hactool_ctx_t tool = {0};
    nca_header_t header = {0};
    nca_fs_header_t* fs = &header.fs_headers[0];
    synthetic_keys(&tool);
    blob_t image = romfs_build(files_a, 3);
    blob_t stored = compress_build(&image, &fs->compression_info);
    blob_t section = ivfc_build(&stored, &fs->romfs_superblock.ivfc_header);
    fs->_0x0 = 2;
    fs->partition_type = PARTITION_ROMFS;
    fs->fs_type = FS_TYPE_ROMFS;
    fs->crypt_type = CRYPT_CTR;
    put64(fs->section_ctr, 0x0000002A00000001ULL);
    section_entry(&header, 0, 0xC00, section.size);
    unsigned char iv[8];
    upper_iv(iv, fs, 0, 0);
    ctr_crypt(iv, 0xC00, section.data, section.size);
    blob_t nca = nca_build(&header, &section);

    nca_ctx_t ctx;
    tool.action = ACTION_VERIFY | ACTION_LISTROMFS;
    process(&ctx, &tool, archive(&nca));
    nca_section_ctx_t* s = &ctx.section_contexts[0];
    CHECK(s->type == ROMFS);
    CHECK(s->compressed != NULL);
    CHECK(s->compressed->size == image.size);
    CHECK(s->compressed_offset == fs->romfs_superblock.ivfc_header.level_headers[IVFC_MAX_LEVEL - 1].logical_offset);
    CHECK(s->superblock_hash_validity == VALIDITY_VALID);
    levels_valid(s->romfs_ctx.ivfc_levels);
    romfs_matches(s, &s->romfs_ctx, NULL, files_a, 3);
    /* The whole decompressed image, across block types, and the stored bytes for hash reads. */
    unsigned char* got = zalloc(image.size > stored.size ? image.size : stored.size);
    nca_section_fseek(s, s->compressed_offset);
    CHECK(nca_section_fread(s, got, image.size) == image.size);
    CHECK(!memcmp(got, image.data, image.size));
    s->uncompressed_reads = 1;
    nca_section_fseek(s, s->compressed_offset);
    CHECK(nca_section_fread(s, got, stored.size) == stored.size);
    CHECK(!memcmp(got, stored.data, stored.size));
    free(got);
    nca_free_section_contexts(&ctx);
    CHECK(!fclose(ctx.file));
    free(image.data);
    free(stored.data);
    free(section.data);
    free(nca.data);
}

/* A base whose RomFS section is sparse, and its update. V is the patched section (IVFC over a compressed RomFS);
   the base holds V with every third CHUNK changed, and keeps only the chunks the update takes from it. The update
   holds the changed chunks, encrypted in three AesCtrEx runs: generation 5, plaintext, generation 9. */
typedef struct {
    blob_t base, patch;
    blob_t image;
} update_t;

static int chunk_patched(size_t k) { return k % 3 == 1; }

static update_t update_build(void) {
    update_t out;
    nca_header_t patch_header = {0}, base_header = {0};
    nca_fs_header_t* pfs = &patch_header.fs_headers[1];
    nca_fs_header_t* bfs = &base_header.fs_headers[1];
    out.image = romfs_build(files_b, 4);
    blob_t stored = compress_build(&out.image, &pfs->compression_info);
    blob_t v = ivfc_build(&stored, &pfs->bktr_superblock.ivfc_header);
    size_t chunks = v.size / CHUNK;
    CHECK(chunks > 2 * ((BUCKET_TREE_NODE_SIZE - 0x10) / INDIRECT_ENTRY_SIZE)); /* three entry sets */

    /* The base, sparse: its physical region keeps the chunks the update does not replace. */
    unsigned char* base_image = zalloc(v.size);
    for (size_t k = 0; k < chunks; k++)
        for (size_t i = 0; i < CHUNK; i++) {
            unsigned char byte = v.data[k * CHUNK + i];
            base_image[k * CHUNK + i] = (unsigned char)(chunk_patched(k) ? ~byte : byte);
        }
    bfs->_0x0 = 2;
    bfs->partition_type = PARTITION_ROMFS;
    bfs->fs_type = FS_TYPE_ROMFS;
    bfs->crypt_type = CRYPT_CTR;
    bfs->romfs_superblock.ivfc_header = pfs->bktr_superblock.ivfc_header;
    put64(bfs->section_ctr, 0x0000007700000002ULL);
    unsigned char iv[8];
    upper_iv(iv, bfs, 0, 0);
    ctr_crypt(iv, 0xC00, base_image, v.size); /* the counter of a sparse section's data is its virtual offset */
    unsigned char* sparse_entries = zalloc(chunks * INDIRECT_ENTRY_SIZE);
    blob_t physical = {zalloc(v.size), 0};
    for (size_t k = 0; k < chunks; k++) {
        if (chunk_patched(k)) {
            indirect_entry(sparse_entries + k * INDIRECT_ENTRY_SIZE, k * CHUNK, 0, 1);
        } else {
            indirect_entry(sparse_entries + k * INDIRECT_ENTRY_SIZE, k * CHUNK, physical.size, 0);
            memcpy(physical.data + physical.size, base_image + k * CHUNK, CHUNK);
            physical.size += CHUNK;
        }
    }
    blob_t sparse = tree_build(sparse_entries, INDIRECT_ENTRY_SIZE, (uint32_t)chunks, v.size);
    nca_sparse_info_t* info = &bfs->sparse_info;
    info->physical_offset = 0xC00;
    info->meta_offset = align_up(physical.size, 0x200);
    info->meta_size = sparse.size;
    info->generation = 0x0102;
    tree_header(&info->meta_header, (uint32_t)chunks);
    upper_iv(iv, bfs, (uint32_t)info->generation << 16, 1);
    ctr_crypt(iv, info->physical_offset + info->meta_offset, sparse.data, sparse.size);
    blob_t base_body = {zalloc(align_up(info->meta_offset + sparse.size, 0x200)), 0};
    base_body.size = align_up(info->meta_offset + sparse.size, 0x200);
    memcpy(base_body.data, physical.data, physical.size);
    memcpy(base_body.data + info->meta_offset, sparse.data, sparse.size);
    section_entry(&base_header, 1, 0xC00, v.size); /* the virtual extent; the bytes are in the physical region */
    out.base = nca_build(&base_header, &base_body);

    /* The update: its chunks, the relocation table, then the AesCtrEx table. */
    unsigned char* reloc_entries = zalloc(chunks * INDIRECT_ENTRY_SIZE);
    blob_t data = {zalloc(v.size), 0};
    for (size_t k = 0; k < chunks; k++) {
        if (chunk_patched(k)) {
            indirect_entry(reloc_entries + k * INDIRECT_ENTRY_SIZE, k * CHUNK, data.size, 1);
            memcpy(data.data + data.size, v.data + k * CHUNK, CHUNK);
            data.size += CHUNK;
        } else {
            indirect_entry(reloc_entries + k * INDIRECT_ENTRY_SIZE, k * CHUNK, k * CHUNK, 0);
        }
    }
    blob_t reloc = tree_build(reloc_entries, INDIRECT_ENTRY_SIZE, (uint32_t)chunks, v.size);
    size_t tables = align_up(data.size, 0x200);
    uint64_t runs[3] = {0, align_up(data.size / 3, 0x10), align_up(2 * data.size / 3, 0x10)};
    unsigned char ctr_ex_entries[3 * AES_CTR_EX_ENTRY_SIZE] = {0};
    for (unsigned r = 0; r < 3; r++) {
        put64(ctr_ex_entries + r * AES_CTR_EX_ENTRY_SIZE, runs[r]);
        ctr_ex_entries[r * AES_CTR_EX_ENTRY_SIZE + 8] = r == 1; /* not encrypted */
        put32(ctr_ex_entries + r * AES_CTR_EX_ENTRY_SIZE + 0xC, r == 0 ? 5 : 9);
    }
    blob_t ctr_ex = tree_build(ctr_ex_entries, AES_CTR_EX_ENTRY_SIZE, 3, tables);
    size_t ctr_ex_offset = tables + reloc.size;
    blob_t body = {0};
    body.size = align_up(ctr_ex_offset + ctr_ex.size, 0x200);
    body.data = zalloc(body.size);
    memcpy(body.data, data.data, data.size);
    memcpy(body.data + tables, reloc.data, reloc.size);
    memcpy(body.data + ctr_ex_offset, ctr_ex.data, ctr_ex.size);
    pfs->_0x0 = 2;
    pfs->partition_type = PARTITION_ROMFS;
    pfs->fs_type = FS_TYPE_ROMFS;
    pfs->crypt_type = CRYPT_BKTR;
    put64(pfs->section_ctr, 0x0000007700000003ULL);
    bktr_superblock_t* sb = &pfs->bktr_superblock;
    sb->relocation_header.offset = tables;
    sb->relocation_header.size = reloc.size;
    sb->relocation_header.magic = MAGIC_BKTR;
    sb->relocation_header._0x14 = 1;
    sb->relocation_header.num_entries = (uint32_t)chunks;
    sb->subsection_header.offset = ctr_ex_offset;
    sb->subsection_header.size = ctr_ex.size;
    sb->subsection_header.magic = MAGIC_BKTR;
    sb->subsection_header._0x14 = 1;
    sb->subsection_header.num_entries = 3;
    for (unsigned r = 0; r < 3; r++) {
        uint64_t end = r < 2 ? runs[r + 1] : tables;
        if (r == 1) continue;
        upper_iv(iv, pfs, r == 0 ? 5 : 9, 1);
        ctr_crypt(iv, 0xC00 + runs[r], body.data + runs[r], end - runs[r]);
    }
    upper_iv(iv, pfs, 0, 0);
    ctr_crypt(iv, 0xC00 + tables, body.data + tables, body.size - tables);
    section_entry(&patch_header, 1, 0xC00, body.size);
    patch_header.title_id |= 0x800;
    out.patch = nca_build(&patch_header, &body);

    free(stored.data);
    free(v.data);
    free(base_image);
    free(sparse_entries);
    free(physical.data);
    free(sparse.data);
    free(base_body.data);
    free(reloc_entries);
    free(data.data);
    free(reloc.data);
    free(ctr_ex.data);
    free(body.data);
    return out;
}

static void update_free(update_t* u) {
    free(u->base.data);
    free(u->patch.data);
    free(u->image.data);
}

static void sparse_update(const update_t* u) {
    hactool_ctx_t base_tool = {0}, tool = {0};
    nca_ctx_t base, patch;
    synthetic_keys(&base_tool);
    synthetic_keys(&tool);
    FILE* base_file = archive(&u->base);
    process(&base, &base_tool, base_file);
    CHECK(base.section_contexts[1].type == ROMFS);
    CHECK(base.section_contexts[1].raw_storage != NULL);

    tool.action = ACTION_VERIFY | ACTION_LISTROMFS;
    tool.base_file = base_file;
    tool.base_file_type = BASEFILE_NCA;
    tool.base_nca_ctx = &base;
    process(&patch, &tool, archive(&u->patch));
    nca_section_ctx_t* s = &patch.section_contexts[1];
    CHECK(s->type == BKTR);
    CHECK(s->bktr_ctx.indirect != NULL);
    CHECK(s->compressed != NULL && s->compressed->size == u->image.size);
    CHECK(s->superblock_hash_validity == VALIDITY_VALID);
    levels_valid(s->bktr_ctx.ivfc_levels); /* the patched section, rebuilt exactly */
    romfs_matches(s, NULL, &s->bktr_ctx, files_b, 4);

    /* --onlyupdated: a replaced chunk is the update's, one in between is the base's. */
    CHECK(storage_indirect_has_patch_data(s->bktr_ctx.indirect, 1 * CHUNK + 3, 4));
    CHECK(!storage_indirect_has_patch_data(s->bktr_ctx.indirect, 3 * CHUNK + 3, 4));
    CHECK(storage_indirect_has_patch_data(s->bktr_ctx.indirect, 3 * CHUNK + 8, CHUNK)); /* into chunk 4 */
    CHECK(!storage_indirect_has_patch_data(s->bktr_ctx.indirect, 2 * CHUNK, 2 * CHUNK));

    /* A physical read is the update's own section, decrypted: its first chunk is V's chunk 1. */
    unsigned char got[CHUNK], want[CHUNK];
    s->uncompressed_reads = 1;
    s->physical_reads = 1;
    nca_section_fseek(s, 0);
    CHECK(nca_section_fread(s, got, sizeof(got)) == sizeof(got));
    s->physical_reads = 0;
    nca_section_fseek(s, CHUNK);
    CHECK(nca_section_fread(s, want, sizeof(want)) == sizeof(want));
    CHECK(!memcmp(got, want, sizeof(got)));
    s->uncompressed_reads = 0;

    nca_free_section_contexts(&patch);
    CHECK(!fclose(patch.file));
    nca_free_section_contexts(&base);
    CHECK(!fclose(base_file));
}

/* An update with no AesCtrEx entries (Ryubing's LibHac): every byte comes from the base. */
static void empty_aes_ctr_ex(void) {
    hactool_ctx_t base_tool = {0}, tool = {0};
    nca_header_t base_header = {0}, patch_header = {0};
    synthetic_keys(&base_tool);
    synthetic_keys(&tool);
    blob_t content = {zalloc(0x2345), 0x2345};
    noise(content.data, content.size, 3);
    romfs_file_t file = {"only.bin", content};
    blob_t image = romfs_build(&file, 1);
    nca_fs_header_t* bfs = &base_header.fs_headers[0];
    blob_t section = ivfc_build(&image, &bfs->romfs_superblock.ivfc_header);
    bfs->_0x0 = 2;
    bfs->partition_type = PARTITION_ROMFS;
    bfs->fs_type = FS_TYPE_ROMFS;
    bfs->crypt_type = CRYPT_CTR;
    put64(bfs->section_ctr, 0x0000001100000001ULL);
    unsigned char iv[8];
    upper_iv(iv, bfs, 0, 0);
    blob_t encrypted = {zalloc(section.size), section.size};
    memcpy(encrypted.data, section.data, section.size);
    ctr_crypt(iv, 0xC00, encrypted.data, encrypted.size);
    section_entry(&base_header, 0, 0xC00, section.size);
    blob_t base = nca_build(&base_header, &encrypted);

    unsigned char entry[INDIRECT_ENTRY_SIZE];
    indirect_entry(entry, 0, 0, 0);
    blob_t reloc = tree_build(entry, INDIRECT_ENTRY_SIZE, 1, section.size);
    blob_t body = {zalloc(align_up(reloc.size + BUCKET_TREE_NODE_SIZE, 0x200)), 0};
    body.size = align_up(reloc.size + BUCKET_TREE_NODE_SIZE, 0x200);
    memcpy(body.data, reloc.data, reloc.size);
    nca_fs_header_t* pfs = &patch_header.fs_headers[0];
    pfs->_0x0 = 2;
    pfs->partition_type = PARTITION_ROMFS;
    pfs->fs_type = FS_TYPE_ROMFS;
    pfs->crypt_type = CRYPT_BKTR;
    pfs->bktr_superblock.ivfc_header = bfs->romfs_superblock.ivfc_header;
    put64(pfs->section_ctr, 0x0000001100000002ULL);
    bktr_superblock_t* sb = &pfs->bktr_superblock;
    sb->relocation_header.size = reloc.size;
    sb->relocation_header.magic = MAGIC_BKTR;
    sb->relocation_header._0x14 = 1;
    sb->relocation_header.num_entries = 1;
    sb->subsection_header.offset = reloc.size;
    sb->subsection_header.size = BUCKET_TREE_NODE_SIZE;
    sb->subsection_header.magic = MAGIC_BKTR;
    sb->subsection_header._0x14 = 1;
    upper_iv(iv, pfs, 0, 0);
    ctr_crypt(iv, 0xC00, body.data, body.size);
    section_entry(&patch_header, 0, 0xC00, body.size);
    blob_t patch = nca_build(&patch_header, &body);

    nca_ctx_t base_ctx, patch_ctx;
    FILE* base_file = archive(&base);
    process(&base_ctx, &base_tool, base_file);
    tool.action = ACTION_VERIFY | ACTION_LISTROMFS;
    tool.base_file = base_file;
    tool.base_file_type = BASEFILE_NCA;
    tool.base_nca_ctx = &base_ctx;
    process(&patch_ctx, &tool, archive(&patch));
    nca_section_ctx_t* s = &patch_ctx.section_contexts[0];
    CHECK(s->type == BKTR && s->bktr_ctx.indirect != NULL);
    CHECK(s->superblock_hash_validity == VALIDITY_VALID);
    levels_valid(s->bktr_ctx.ivfc_levels);
    romfs_matches(s, NULL, &s->bktr_ctx, &file, 1);
    nca_free_section_contexts(&patch_ctx);
    CHECK(!fclose(patch_ctx.file));
    nca_free_section_contexts(&base_ctx);
    CHECK(!fclose(base_file));
    free(content.data);
    free(image.data);
    free(section.data);
    free(encrypted.data);
    free(base.data);
    free(reloc.data);
    free(body.data);
    free(patch.data);
}

/* Past 2046 entry sets, a tree has L2 nodes between its L1 node and its entry sets (LibHac's GetNodeL2Count). */
static void two_level_tree(void) {
    const uint32_t per_set = (BUCKET_TREE_NODE_SIZE - 0x10) / INDIRECT_ENTRY_SIZE;
    const uint32_t offsets = (BUCKET_TREE_NODE_SIZE - 0x10) / 8;
    const uint32_t full = per_set * offsets; /* as many entry sets as the L1 node has offsets */
    const uint64_t node = BUCKET_TREE_NODE_SIZE;
    CHECK(per_set == 818 && offsets == 2046);
    CHECK(bucket_tree_query_node_storage_size(INDIRECT_ENTRY_SIZE, full) == node);
    CHECK(bucket_tree_query_node_storage_size(INDIRECT_ENTRY_SIZE, full + 1) == 2 * node);
    CHECK(bucket_tree_query_node_storage_size(INDIRECT_ENTRY_SIZE, 2 * full) == 3 * node);
    CHECK(bucket_tree_query_entry_storage_size(INDIRECT_ENTRY_SIZE, full + 1) == (offsets + 1) * node);

    /* Runs of 16 bytes alternating between two storages, read across entry sets and past the L2 boundary. */
    uint32_t count = full + 5;
    unsigned char* entries = zalloc((size_t)count * INDIRECT_ENTRY_SIZE);
    for (uint32_t i = 0; i < count; i++)
        indirect_entry(entries + (size_t)i * INDIRECT_ENTRY_SIZE, 16ULL * i, 16ULL * i, i & 1);
    blob_t tree = tree_build(entries, INDIRECT_ENTRY_SIZE, count, 16ULL * count);
    FILE* file = archive(&tree);
    storage_t* table = storage_file_new(file, 0, tree.size);
    bucket_tree_t* loaded = bucket_tree_load(table, 0, tree.size, INDIRECT_ENTRY_SIZE, count);
    CHECK(loaded != NULL);
    storage_t* original = storage_fill_new(UINT64_MAX, 0xAA);
    storage_t* patch = storage_fill_new(UINT64_MAX, 0x55);
    storage_t* indirect = storage_indirect_new(original, patch, loaded);
    CHECK(indirect->size == 16ULL * count);
    const uint32_t at[] = {0, per_set - 1, per_set, full - 1, full, count - 1};
    for (unsigned j = 0; j < sizeof(at) / sizeof(at[0]); j++) {
        unsigned char got[24];
        uint64_t offset = 16ULL * at[j] + 4;
        size_t size = at[j] == count - 1 ? 12 : sizeof(got);
        CHECK(storage_read(indirect, offset, got, size));
        for (size_t b = 0; b < size; b++) CHECK(got[b] == (((offset + b) / 16) & 1 ? 0x55 : 0xAA));
    }
    CHECK(!storage_read(indirect, 16ULL * count - 4, (unsigned char[8]){0}, 8)); /* past the end */

    /* A table whose header claims more entries than its sets hold is refused. */
    CHECK(bucket_tree_load(table, 0, tree.size, INDIRECT_ENTRY_SIZE, count + 1) == NULL);
    storage_free(indirect);
    storage_free(patch);
    storage_free(original);
    storage_free(table);
    CHECK(!fclose(file));
    free(tree.data);
    free(entries);
}

/* A PFS0 section in AES-XTS, its sectors numbered from the section's start, read across sector boundaries. */
static void xts_partition(void) {
    hactool_ctx_t tool = {0};
    nca_header_t header = {0};
    nca_fs_header_t* fs = &header.fs_headers[0];
    synthetic_keys(&tool);
    blob_t section = {zalloc(0x1000), 0x1000};
    pfs0_header_t* pfs = (pfs0_header_t*)(section.data + 0x200);
    pfs->magic = MAGIC_PFS0;
    noise(section.data + 0x200 + sizeof(*pfs), 0x900, 21); /* past the empty partition, in the hashed data */
    pfs0_superblock_t* sb = &fs->pfs0_superblock;
    sb->block_size = 0x400;
    sb->always_2 = 2;
    sb->hash_table_offset = 0;
    sb->hash_table_size = 0x40;
    sb->pfs0_offset = 0x200;
    sb->pfs0_size = 0x800;
    sha256_hash_buffer(section.data, section.data + 0x200, 0x400);
    sha256_hash_buffer(section.data + 0x20, section.data + 0x600, 0x400);
    sha256_hash_buffer(sb->master_hash, section.data, 0x40);
    blob_t plain = {zalloc(section.size), section.size};
    memcpy(plain.data, section.data, section.size);
    fs->_0x0 = 2;
    fs->partition_type = PARTITION_PFS0;
    fs->fs_type = FS_TYPE_PFS0;
    fs->crypt_type = CRYPT_XTS;
    section_entry(&header, 0, 0xC00, section.size);
    aes_ctx_t* aes = new_aes_ctx(section_keys, 32, AES_MODE_XTS);
    aes_xts_encrypt(aes, section.data, section.data, section.size, 0, 0x200);
    free_aes_ctx(aes);
    blob_t nca = nca_build(&header, &section);

    nca_ctx_t ctx;
    tool.action = ACTION_VERIFY;
    process(&ctx, &tool, archive(&nca));
    nca_section_ctx_t* s = &ctx.section_contexts[0];
    CHECK(s->type == PFS0);
    CHECK(s->superblock_hash_validity == VALIDITY_VALID);
    CHECK(s->pfs0_ctx.hash_table_validity == VALIDITY_VALID);
    unsigned char got[0x333];
    nca_section_fseek(s, 0x1F7);
    CHECK(nca_section_fread(s, got, sizeof(got)) == sizeof(got));
    CHECK(!memcmp(got, plain.data + 0x1F7, sizeof(got)));
    free(s->pfs0_ctx.header);
    nca_free_section_contexts(&ctx);
    CHECK(!fclose(ctx.file));
    free(section.data);
    free(plain.data);
    free(nca.data);
}

/* A gamecard dump that starts with the card's key area: the header and every offset in it come after. */
static void gamecard_key_area(void) {
    hactool_ctx_t tool = {0};
    xci_ctx_t ctx = {0};
    size_t card_size = XCI_KEY_AREA_SIZE + 0x200 + sizeof(hfs0_header_t);
    blob_t card = {zalloc(card_size), card_size};
    xci_header_t* header = (xci_header_t*)(card.data + XCI_KEY_AREA_SIZE);
    hfs0_header_t* root = (hfs0_header_t*)(card.data + XCI_KEY_AREA_SIZE + 0x200);
    noise(card.data, 0x200, 11); /* the initial data */
    root->magic = MAGIC_HFS0;
    header->magic = MAGIC_HEAD;
    header->hfs0_offset = 0x200;
    header->hfs0_header_size = sizeof(*root);
    sha256_hash_buffer(header->hfs0_header_hash, root, sizeof(*root));
    sha256_hash_buffer(header->crypto_header_hash, card.data, 0x200);
    ctx.tool_ctx = &tool;
    ctx.file = archive(&card);
    xci_process(&ctx);
    CHECK(ctx.key_area_size == XCI_KEY_AREA_SIZE);
    CHECK(ctx.initial_data_validity == VALIDITY_VALID);
    CHECK(ctx.header.magic == MAGIC_HEAD);
    CHECK(ctx.hfs0_hash_validity == VALIDITY_VALID);
    CHECK(ctx.partition_ctx.offset == XCI_KEY_AREA_SIZE + 0x200);
    CHECK(ctx.partition_ctx.header != NULL && ctx.partition_ctx.header->magic == MAGIC_HFS0);
    free(ctx.partition_ctx.header);
    CHECK(!fclose(ctx.file));
    free(card.data);
}

/* The command line scarletnx --extract runs: --basenca and --romfsdir, keys from a file. */
static void write_file(const char* path, const void* data, size_t size) {
    FILE* file = fopen(path, "wb");
    CHECK(file != NULL);
    CHECK(fwrite(data, 1, size, file) == size);
    CHECK(!fclose(file));
}

static void hex(char* out, const unsigned char* key, size_t size) {
    for (size_t i = 0; i < size; i++) sprintf(out + 2 * i, "%02x", key[i]);
}

static void command_line(const char* hactool, const update_t* u) {
    const char* dir = "hactool_layers.tmp";
    char keys[256], a[80], b[80], arg_base[64], arg_out[64], path[128];
#if defined(_WIN32)
    _mkdir(dir);
#else
    mkdir(dir, 0700);
#endif
    hex(a, header_key, sizeof(header_key));
    hex(b, key_area_key, sizeof(key_area_key));
    sprintf(keys, "header_key = %s\nkey_area_key_application_00 = %s\n", a, b);
    sprintf(path, "%s/prod.keys", dir);
    write_file(path, keys, strlen(keys));
    sprintf(path, "%s/base.nca", dir);
    write_file(path, u->base.data, u->base.size);
    sprintf(path, "%s/patch.nca", dir);
    write_file(path, u->patch.data, u->patch.size);
    sprintf(arg_base, "--basenca=%s/base.nca", dir);
    sprintf(arg_out, "--romfsdir=%s/romfs", dir);
    char key_path[96], patch_path[96];
    sprintf(key_path, "%s/prod.keys", dir);
    sprintf(patch_path, "%s/patch.nca", dir);
    CHECK(!fflush(NULL));
#if defined(_WIN32)
    char* quoted = zalloc(strlen(hactool) + 3);
    sprintf(quoted, "\"%s\"", hactool);
    const char* args[] = {quoted, "--disablekeywarns", "-k", key_path, arg_base, arg_out, patch_path, NULL};
    CHECK(_spawnv(_P_WAIT, hactool, args) == 0);
    free(quoted);
#else
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        execl(hactool, hactool, "--disablekeywarns", "-k", key_path, arg_base, arg_out, patch_path, (char*)NULL);
        _exit(127);
    }
    int status;
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
#endif
    for (unsigned i = 0; i < 4; i++) {
        sprintf(path, "%s/romfs/%s", dir, files_b[i].name);
        FILE* file = fopen(path, "rb");
        CHECK(file != NULL);
        unsigned char* got = zalloc(files_b[i].content.size + 1);
        CHECK(fread(got, 1, files_b[i].content.size + 1, file) == files_b[i].content.size);
        CHECK(!memcmp(got, files_b[i].content.data, files_b[i].content.size));
        CHECK(!fclose(file));
        CHECK(!remove(path));
        free(got);
    }
    sprintf(path, "%s/romfs", dir);
    CHECK(!rmdir(path));
    CHECK(!remove(key_path));
    CHECK(!remove(patch_path));
    sprintf(path, "%s/base.nca", dir);
    CHECK(!remove(path));
    CHECK(!rmdir(dir));
}

int main(int argc, char** argv) {
    CHECK(sizeof(nca_fs_header_t) == 0x200);
    CHECK(sizeof(nca_sparse_info_t) == 0x30 && sizeof(nca_compression_info_t) == 0x28);
    CHECK(offsetof(nca_fs_header_t, sparse_info) == 0x148);
    CHECK(offsetof(nca_fs_header_t, compression_info) == 0x178);
    CHECK(offsetof(nca_fs_header_t, metadata_hash_info) == 0x1A0);
    make_files();
    compressed_romfs();
    update_t update = update_build();
    sparse_update(&update);
    empty_aes_ctr_ex();
    two_level_tree();
    xts_partition();
    gamecard_key_area();
    if (argc > 1) command_line(argv[1], &update);
    update_free(&update);
    printf("hactool layers: %u checks passed%s\n", checks, argc > 1 ? ", with the command line" : "");
    return 0;
}
