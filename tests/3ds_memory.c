/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "memory/ctr_memory.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef struct { unsigned reads, writes; uint32_t address; unsigned char data[8]; bool fail; } Device;
static bool read_mmio(void* context, uint32_t pa, void* output, size_t size) {
    Device* d = context;
    d->reads++; d->address = pa;
    if (d->fail) return false;
    memcpy(output, d->data, size);
    return true;
}
static bool write_mmio(void* context, uint32_t pa, const void* input, size_t size) {
    Device* d = context;
    d->writes++; d->address = pa;
    if (d->fail) return false;
    memcpy(d->data, input, size);
    return true;
}
int main(void) {
    assert(!snx_ctr_memory_new((SnxCtrModel)99) && !snx_ctr_space_new(NULL));
    for (unsigned model = 0; model < 2; ++model) {
        SnxCtrMemory* m = snx_ctr_memory_new((SnxCtrModel)model);
        SnxCtrAddressSpace *s = snx_ctr_space_new(m), *alias = snx_ctr_space_new(m);
        assert(m && s && alias);
        unsigned char input[8] = {1, 2, 3, 4, 5, 6, 7, 8}, output[8] = {0};
        assert(snx_ctr_map(s, 0x1000, 0x20000000, 0x1000, 3) == SNX_CTR_MEM_OK);
        assert(snx_ctr_map(s, 0x2000, 0x20003000, 0x1000, 3) == SNX_CTR_MEM_OK);
        assert(snx_ctr_map(alias, 0x8000, 0x20000000, 0x1000, 5) == SNX_CTR_MEM_OK);
        assert(snx_ctr_map(alias, 0x9000, 0x20003000, 0x1000, 1) == SNX_CTR_MEM_OK);
        assert(snx_ctr_read(s, 0x1000, output, 8) == SNX_CTR_MEM_OK && !memcmp(output, (char[8]){0}, 8));
        assert(snx_ctr_write(s, 0x1ffd, input, 8) == SNX_CTR_MEM_OK);
        assert(snx_ctr_read(alias, 0x8ffd, output, 8) == SNX_CTR_MEM_OK && !memcmp(input, output, 8));
        assert(snx_ctr_fetch(alias, 0x8ffd, output, 3) == SNX_CTR_MEM_OK);
        memset(output, 0xff, 8);
        assert(snx_ctr_fetch(alias, 0x8ffd, output, 8) == SNX_CTR_MEM_PERMISSION && output[0] == 0xff);
        assert(snx_ctr_write(alias, 0x8000, input, 1) == SNX_CTR_MEM_PERMISSION);
        assert(snx_ctr_write(s, 0x2ffd, input, 8) == SNX_CTR_MEM_UNMAPPED);
        assert(snx_ctr_read(s, 0x2ffd, output, 3) == SNX_CTR_MEM_OK && !output[0]);
        assert(snx_ctr_protect(s, 0x2000, 0x1000, 1) == SNX_CTR_MEM_OK);
        unsigned char changed[8] = {9, 9, 9, 9, 9, 9, 9, 9};
        assert(snx_ctr_write(s, 0x1ffd, changed, 8) == SNX_CTR_MEM_PERMISSION);
        assert(snx_ctr_read(alias, 0x8ffd, output, 8) == SNX_CTR_MEM_OK && !memcmp(output, input, 8));
        assert(snx_ctr_protect(s, 0x1000, 0x2000, 1) == SNX_CTR_MEM_OK);
        assert(snx_ctr_write(s, 0x1ffd, input, 8) == SNX_CTR_MEM_PERMISSION);
        assert(snx_ctr_protect(s, 0x1000, 0x3000, 7) == SNX_CTR_MEM_UNMAPPED);
        unsigned permissions = 0;
        uint32_t pa = 0;
        assert(snx_ctr_query(s, 0x1003, &pa, &permissions) == SNX_CTR_MEM_OK && pa == 0x20000003 && permissions == 1);
        assert(snx_ctr_unmap(s, 0x1000, 0x3000) == SNX_CTR_MEM_UNMAPPED);
        assert(snx_ctr_query(s, 0x1000, NULL, NULL) == SNX_CTR_MEM_OK);
        assert(snx_ctr_map(s, 0, 0x20000000, 0x2000, 3) == SNX_CTR_MEM_OVERLAP);
        assert(snx_ctr_query(s, 0, NULL, NULL) == SNX_CTR_MEM_UNMAPPED);
        assert(snx_ctr_map(s, 1, 0x20000000, 0x1000, 3) == SNX_CTR_MEM_INVALID);
        assert(snx_ctr_map(s, 0, 0x20000001, 0x1000, 3) == SNX_CTR_MEM_INVALID);
        assert(snx_ctr_map(s, 0, 0x20000000, 0, 3) == SNX_CTR_MEM_INVALID);
        assert(snx_ctr_map(s, 0, 0x20000000, 0x1000, 8) == SNX_CTR_MEM_INVALID);
        assert(snx_ctr_map(s, 0xfffff000, 0x20000000, 0x2000, 3) == SNX_CTR_MEM_INVALID);
        assert(snx_ctr_map(s, 0xfffff000, 0x20000000, 0x1000, 3) == SNX_CTR_MEM_OK);
        assert(snx_ctr_write(s, 0xffffffff, input, 1) == SNX_CTR_MEM_OK);
        assert(snx_ctr_write(s, 0xffffffff, input, 2) == SNX_CTR_MEM_INVALID);
        assert(snx_ctr_read(s, 0, output, 0) == SNX_CTR_MEM_INVALID);
        assert(snx_ctr_read(s, 0x1000, NULL, 1) == SNX_CTR_MEM_INVALID);
        assert(snx_ctr_map(s, 0x4000, 0x17fff000, 0x2000, 3) == SNX_CTR_MEM_INVALID);
        assert(snx_ctr_map(s, 0x4000, 0x1f000000, 0x1000, 3) == (model ? SNX_CTR_MEM_OK : SNX_CTR_MEM_INVALID));
        assert(snx_ctr_map(s, 0x100000, 0x28000000, 0x1000, 3) == (model ? SNX_CTR_MEM_OK : SNX_CTR_MEM_INVALID));
        assert(snx_ctr_map(s, 0x5000, 0x10100000, 0x1000, 7) == SNX_CTR_MEM_PERMISSION);
        assert(snx_ctr_map(s, 0x5000, 0x10100000, 0x1000, 3) == SNX_CTR_MEM_OK);
        assert(snx_ctr_protect(s, 0x5000, 0x1000, 5) == SNX_CTR_MEM_PERMISSION);
        assert(snx_ctr_read(s, 0x5000, output, 4) == SNX_CTR_MEM_DEVICE);
        Device d = {0};
        snx_ctr_memory_mmio(m, &d, read_mmio, write_mmio);
        assert(snx_ctr_write(s, 0x5010, input, 4) == SNX_CTR_MEM_OK && d.writes == 1 && d.address == 0x10100010);
        assert(snx_ctr_read(s, 0x5010, output, 4) == SNX_CTR_MEM_OK && d.reads == 1 && !memcmp(input, output, 4));
        assert(snx_ctr_read(s, 0x5001, output, 4) == SNX_CTR_MEM_DEVICE && d.reads == 1);
        assert(snx_ctr_read(s, 0x5ffc, output, 8) == SNX_CTR_MEM_DEVICE && d.reads == 1);
        assert(snx_ctr_map(s, 0x6000, 0x20002000, 0x1000, 3) == SNX_CTR_MEM_OK);
        assert(snx_ctr_map(s, 0x7000, 0x10101000, 0x1000, 3) == SNX_CTR_MEM_OK);
        assert(snx_ctr_write(s, 0x6ffc, input, 8) == SNX_CTR_MEM_DEVICE && d.writes == 1);
        assert(snx_ctr_protect(s, 0x6000, 0x2000, 5) == SNX_CTR_MEM_PERMISSION);
        assert(snx_ctr_query(s, 0x6000, NULL, &permissions) == SNX_CTR_MEM_OK && permissions == 3);
        d.fail = true;
        assert(snx_ctr_read(s, 0x5000, output, 4) == SNX_CTR_MEM_DEVICE && d.reads == 2);
        const SnxCtrProfile* profile = snx_ctr_profile((SnxCtrModel)model);
        for (size_t i = 0; i < profile->region_count; ++i) {
            const SnxCtrRegion* region = &profile->regions[i];
            if (region->kind == SNX_CTR_MMIO) continue;
            uint32_t a = 0x500000 + (uint32_t)i * 4096, b = 0x600000 + (uint32_t)i * 4096;
            unsigned char value = (unsigned char)(0x40 + i);
            assert(snx_ctr_map(s, a, region->base, 4096, 3) == SNX_CTR_MEM_OK);
            assert(snx_ctr_map(alias, b, region->base, 4096, 1) == SNX_CTR_MEM_OK);
            assert(snx_ctr_write(s, a, &value, 1) == SNX_CTR_MEM_OK);
            assert(snx_ctr_read(alias, b, output, 1) == SNX_CTR_MEM_OK && output[0] == value);
        }
        assert(snx_ctr_unmap(s, 0x1000, 0x2000) == SNX_CTR_MEM_OK);
        assert(snx_ctr_read(s, 0x1000, output, 1) == SNX_CTR_MEM_UNMAPPED);
        assert(snx_ctr_query(alias, 0x8000, NULL, NULL) == SNX_CTR_MEM_OK);
        snx_ctr_space_free(alias); snx_ctr_space_free(s); snx_ctr_memory_free(m);
    }
    snx_ctr_space_free(NULL); snx_ctr_memory_free(NULL);
    puts("3DS page mappings, aliases, permissions, bounds, transactions and MMIO passed");
    return 0;
}
