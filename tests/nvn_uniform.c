/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gpu/nvn_uniform.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static NvnUniformUpdate* update(NvnUniformUpdate* previous, u64 address, const u8* bytes, u32 size) {
    NvnUniformUpdate* result = malloc(sizeof *result + size);
    assert(result);
    result->next = previous;
    result->address = address;
    result->size = size;
    memcpy(result->bytes, bytes, size);
    return result;
}

int main(void) {
    const u8 first[] = {1, 2, 3, 4};
    const u8 second[] = {9, 8, 7};
    const u8 unrelated[] = {5};
    NvnUniformUpdate* old = update(NULL, 0x1002, first, sizeof first);
    NvnUniformUpdate* recent = update(old, 0x1004, second, sizeof second);
    NvnUniformUpdate* head = update(recent, 0x2000, unrelated, sizeof unrelated);
    u8 data[8];
    memset(data, 0xaa, sizeof data);
    assert(nvn_uniform_updates_apply(head, 0x1000, sizeof data, data));
    const u8 expected[] = {0xaa, 0xaa, 1, 2, 9, 8, 7, 0xaa};
    assert(!memcmp(data, expected, sizeof expected));
    u8 clipped[3] = {0};
    assert(nvn_uniform_updates_apply(head, 0x1003, sizeof clipped, clipped));
    const u8 expected_clipped[] = {2, 9, 8};
    assert(!memcmp(clipped, expected_clipped, sizeof expected_clipped));
    assert(!nvn_uniform_updates_apply(head, UINT64_MAX, 2, data));
    head->address = UINT64_MAX;
    assert(!nvn_uniform_updates_apply(head, 0x1000, sizeof data, data));
    free(head);
    free(recent);
    free(old);
    return 0;
}
