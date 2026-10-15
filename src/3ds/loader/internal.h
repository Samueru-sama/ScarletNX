/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CTR_LOADER_INTERNAL_H
#define SNX_CTR_LOADER_INTERNAL_H
#include "ctr_loader.h"
#include "common.h"
#include <openssl/evp.h>
#include <stdio.h>
#define CTR_METADATA_LIMIT (16u * 1024u * 1024u)
#define CTR_CODE_LIMIT     (64u * 1024u * 1024u)
#define CTR_OUTPUT_LIMIT   UINT64_C(1073741824)
typedef struct {
    uint64_t offset, size;
} CtrSpan;
typedef struct {
    FILE* input;
    uint64_t size, output_bytes;
    size_t entries;
    SnxCtrExtractResult result;
    char* error;
    size_t error_size;
} CtrReader;
typedef struct {
    uint64_t title_id;
    uint16_t version;
    unsigned char input_hash[32];
    CtrSpan cert, ticket, tmd, meta, boot;
    unsigned char *header, *metadata;
    size_t chunks, body;
} CtrCia;
static inline uint16_t ctr_le16(const unsigned char* p) { return (uint16_t)(p[0] | p[1] << 8); }
static inline uint32_t ctr_le32(const unsigned char* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static inline uint64_t ctr_le64(const unsigned char* p) { return ctr_le32(p) | (uint64_t)ctr_le32(p + 4) << 32; }
static inline uint16_t ctr_be16(const unsigned char* p) { return (uint16_t)(p[1] | p[0] << 8); }
static inline uint32_t ctr_be32(const unsigned char* p) {
    return (uint32_t)p[3] | (uint32_t)p[2] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[0] << 24;
}
static inline uint64_t ctr_be64(const unsigned char* p) { return ctr_be32(p + 4) | (uint64_t)ctr_be32(p) << 32; }
static inline bool ctr_fits(uint64_t total, uint64_t at, uint64_t size) { return at <= total && size <= total - at; }
bool ctr_fail(CtrReader* r, SnxCtrExtractResult result, const char* format, ...);
bool ctr_read(CtrReader* r, CtrSpan span, uint64_t at, void* output, size_t size);
bool ctr_hash(CtrReader* r, CtrSpan span, unsigned char hash[32]);
bool ctr_sha256(CtrReader* r, const void* data, size_t size, unsigned char hash[32]);
bool ctr_hash_matches(CtrReader* r, CtrSpan span, const unsigned char expected[32]);
bool ctr_copy(CtrReader* r, CtrSpan span, const char* path, const unsigned char* expected);
bool ctr_write(CtrReader* r, const char* path, const void* data, size_t size);
bool ctr_directory(CtrReader* r, const char* path);
bool ctr_name(const char* name);
bool ctr_cia_load(CtrReader* r, CtrCia* cia);
bool ctr_cia_contents(CtrReader* r, const CtrCia* cia, const char* stage);
void ctr_cia_free(CtrCia* cia);
bool ctr_ncch_extract(CtrReader* r, const CtrCia* cia, const char* stage);
bool ctr_romfs_extract(CtrReader* r, CtrSpan romfs, const char* stage);
#endif
