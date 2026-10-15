/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Format reference: Azahar CIAContainer, Ticket, TitleMetadata at
   68202bc4ff7edf9d7280ec0dcd6572b7cd1ae26e (GPL-3.0-or-later).
   New bounds-checked streaming implementation; signatures are NOT authenticated. */
#include "internal.h"
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

static uint64_t align64(uint64_t n) { return (n + 63) & ~UINT64_C(63); }
static size_t body_offset(uint32_t signature) {
    switch (signature) {
        case 0x10000:
        case 0x10003: return 0x240;
        case 0x10001:
        case 0x10004: return 0x140;
        case 0x10002:
        case 0x10005: return 0x40;
        default: return 0;
    }
}
static bool bounded(CtrReader* r, CtrSpan span) {
    return ctr_fits(r->size, span.offset, span.size) ||
           ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "CIA section exceeds input size");
}
static int compare_ids(const void* a, const void* b) {
    uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
    return (x > y) - (x < y);
}
bool ctr_cia_load(CtrReader* r, CtrCia* cia) {
    if (r->size > CTR_OUTPUT_LIMIT) return ctr_fail(r, SNX_CTR_EXTRACT_LIMIT, "CIA exceeds 1 GiB input limit");
    cia->header = malloc(0x2020);
    if (!cia->header) return ctr_fail(r, SNX_CTR_EXTRACT_LIMIT, "out of memory");
    if (!ctr_read(r, (CtrSpan){0, r->size}, 0, cia->header, 0x2020)) return false;
    const unsigned char* h = cia->header;
    if (ctr_le32(h) != 0x2020 || ctr_le16(h + 4) || ctr_le16(h + 6))
        return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "unsupported CIA header/type/version");
    cia->cert = (CtrSpan){0x2040, ctr_le32(h + 8)};
    cia->ticket = (CtrSpan){align64(cia->cert.offset + cia->cert.size), ctr_le32(h + 12)};
    cia->tmd = (CtrSpan){align64(cia->ticket.offset + cia->ticket.size), ctr_le32(h + 16)};
    CtrSpan content = {align64(cia->tmd.offset + cia->tmd.size), ctr_le64(h + 24)};
    if (!bounded(r, cia->cert) || !bounded(r, cia->ticket) || !bounded(r, cia->tmd) || !bounded(r, content))
        return false;
    cia->meta = (CtrSpan){align64(content.offset + content.size), ctr_le32(h + 20)};
    if (cia->meta.size && (!bounded(r, cia->meta) || cia->meta.size < 0x400))
        return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "invalid CIA metadata size");
    if (cia->tmd.size > CTR_METADATA_LIMIT || cia->ticket.size > CTR_METADATA_LIMIT)
        return ctr_fail(r, SNX_CTR_EXTRACT_LIMIT, "CIA metadata exceeds 16 MiB limit");
    unsigned char sig[4];
    if (!ctr_read(r, cia->ticket, 0, sig, sizeof sig)) return false;
    size_t ticket_body = body_offset(ctr_be32(sig));
    if (!ticket_body || !ctr_fits(cia->ticket.size, ticket_body, 0x164))
        return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "invalid ticket signature/body size");
    unsigned char ticket_id[8], rights[8];
    if (!ctr_read(r, cia->ticket, ticket_body + 0x9c, ticket_id, sizeof ticket_id) ||
        !ctr_read(r, cia->ticket, ticket_body + 0x164, rights, sizeof rights))
        return false;
    uint32_t rights_size = ctr_be32(rights + 4);
    if (rights_size < 8 || !ctr_fits(cia->ticket.size, ticket_body + 0x164, rights_size))
        return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "invalid ticket content-index size");
    cia->metadata = malloc((size_t)cia->tmd.size);
    if (!cia->metadata) return ctr_fail(r, SNX_CTR_EXTRACT_LIMIT, "out of memory");
    if (!ctr_read(r, cia->tmd, 0, cia->metadata, (size_t)cia->tmd.size)) return false;
    if (cia->tmd.size < 4 || !(cia->body = body_offset(ctr_be32(cia->metadata))) ||
        !ctr_fits(cia->tmd.size, cia->body, 0x9c4))
        return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "invalid TMD signature/body size");
    const unsigned char* body = cia->metadata + cia->body;
    cia->title_id = ctr_be64(body + 0x4c);
    cia->version = ctr_be16(body + 0x9c);
    cia->chunks = ctr_be16(body + 0x9e);
    uint16_t boot = ctr_be16(body + 0xa0);
    if (cia->title_id != ctr_be64(ticket_id))
        return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "ticket/TMD title ID mismatch");
    if ((cia->title_id >> 32) != 0x00040000)
        return ctr_fail(r, SNX_CTR_EXTRACT_UNSUPPORTED,
                        "only application CIAs are supported; not updates/DLC/system titles");
    if (!cia->chunks || !ctr_fits(cia->tmd.size, cia->body + 0x9c4, cia->chunks * 0x30))
        return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "invalid TMD content count");
    const unsigned char* chunks = body + 0x9c4;
    unsigned char hash[32], covered[65536] = {0}, indices[8192] = {0}, present[8192] = {0};
    if (!ctr_sha256(r, body + 0xc4, 64 * 0x24, hash)) return false;
    if (memcmp(hash, body + 0xa4, 32)) return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "TMD content-info SHA-256 mismatch");
    for (size_t i = 0; i < 64; ++i) {
        const unsigned char* info = body + 0xc4 + i * 0x24;
        size_t first = ctr_be16(info), count = ctr_be16(info + 2);
        if (!count) continue;
        if (first >= cia->chunks || count > cia->chunks - first)
            return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "TMD content-info range exceeds chunk table");
        for (size_t j = first; j < first + count; ++j) {
            if (covered[j]) return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "overlapping TMD content-info ranges");
            covered[j] = 1;
        }
        if (!ctr_sha256(r, chunks + first * 0x30, count * 0x30, hash)) return false;
        if (memcmp(hash, info + 4, 32)) return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "TMD chunk-table SHA-256 mismatch");
    }
    uint32_t* ids = malloc(cia->chunks * sizeof *ids);
    if (!ids) return ctr_fail(r, SNX_CTR_EXTRACT_LIMIT, "out of memory");
    bool ok = true, have_boot = false;
    uint64_t at = 0;
    for (size_t i = 0; i < cia->chunks && ok; ++i) {
        const unsigned char* chunk = chunks + i * 0x30;
        unsigned index = ctr_be16(chunk + 4), bit = 0x80u >> (index & 7);
        ids[i] = ctr_be32(chunk);
        if (!covered[i] || (indices[index >> 3] & bit)) {
            ok = ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "uncovered or duplicate TMD content index");
            break;
        }
        indices[index >> 3] |= (unsigned char)bit;
        if (!(h[0x20 + (index >> 3)] & bit)) continue;
        present[index >> 3] |= (unsigned char)bit;
        uint64_t size = ctr_be64(chunk + 8);
        if (!size || !ctr_fits(content.size, at, size)) {
            ok = ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "invalid CIA content length");
            break;
        }
        if (ctr_be16(chunk + 6) & 1) {
            ok = ctr_fail(r, SNX_CTR_EXTRACT_UNSUPPORTED, "encrypted CIA content is not supported");
            break;
        }
        if (index == boot) {
            cia->boot = (CtrSpan){content.offset + at, size};
            have_boot = true;
        }
        at += size;
    }
    if (ok) {
        qsort(ids, cia->chunks, sizeof *ids, compare_ids);
        for (size_t i = 1; i < cia->chunks; ++i)
            if (ids[i] == ids[i - 1]) {
                ok = ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "duplicate TMD content ID");
                break;
            }
    }
    free(ids);
    if (!ok) return false;
    if (!have_boot || at != content.size || memcmp(present, h + 0x20, sizeof present))
        return ctr_fail(r, SNX_CTR_EXTRACT_FORMAT, "CIA bitmap/content sizes or boot content do not match TMD");
    return ctr_hash(r, (CtrSpan){0, r->size}, cia->input_hash);
}
bool ctr_cia_contents(CtrReader* r, const CtrCia* cia, const char* stage) {
    char* path = snx_path_join(stage, "contents");
    bool ok = ctr_directory(r, path);
    free(path);
    uint64_t at = align64(cia->tmd.offset + cia->tmd.size);
    for (size_t i = 0; i < cia->chunks && ok; ++i) {
        const unsigned char* chunk = cia->metadata + cia->body + 0x9c4 + i * 0x30;
        unsigned index = ctr_be16(chunk + 4);
        if (!(cia->header[0x20 + (index >> 3)] & (0x80u >> (index & 7)))) continue;
        CtrSpan span = {at, ctr_be64(chunk + 8)};
        char* name = snx_format("contents/%08" PRIx32 ".app", ctr_be32(chunk));
        path = snx_path_join(stage, name);
        ok = ctr_copy(r, span, path, chunk + 16);
        free(name);
        free(path);
        at += span.size;
    }
    path = snx_path_join(stage, "cia-header.bin");
    ok = ok && ctr_write(r, path, cia->header, 0x2020);
    free(path);
    const CtrSpan spans[] = {cia->cert, cia->ticket, cia->tmd, cia->meta};
    const char* names[] = {"certificates.bin", "ticket.bin", "tmd.bin", "metadata.bin"};
    for (size_t i = 0; i < 4 && ok; ++i) {
        if (!spans[i].size) continue;
        path = snx_path_join(stage, names[i]);
        ok = ctr_copy(r, spans[i], path, NULL);
        free(path);
    }
    return ok;
}
void ctr_cia_free(CtrCia* cia) {
    free(cia->header);
    free(cia->metadata);
}
