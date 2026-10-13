/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gpu/nvn_command_gap.h"
#include <assert.h>
#include <string.h>

int main(void) {
    /* Observed TotK cursor gap: two state packets followed by its 1,656-index MME draw. */
    const u32 packet[] = {0x20020701, 6, 0x22ad0000, 0x200207c0, 6, 0x22ad0d8b,
                          0xa0050e38, 4, 6, 0x22ad0d90, 1, 1656};
    NvnGapIndexedDraw draw = {0};
    assert(nvn_command_gap_indexed_draw(packet, 12, &draw));
    assert(draw.primitive == 4 && draw.index_type == 1 && draw.count == 1656);
    assert(draw.index_address == 0x622ad0d90ull);

    u32 altered[12];
    memcpy(altered, packet, sizeof altered);
    altered[11] = 3384;
    assert(nvn_command_gap_indexed_draw(altered, 12, &draw) && draw.count == 3384);
    memcpy(altered, packet, sizeof altered);
    altered[6] = 0xa0050e18; /* another MME method is not an indexed draw */
    assert(!nvn_command_gap_indexed_draw(altered, 12, &draw));
    memcpy(altered, packet, sizeof altered);
    altered[10] = 3; /* unsupported index width */
    assert(!nvn_command_gap_indexed_draw(altered, 12, &draw));
    memcpy(altered, packet, sizeof altered);
    altered[0] = 0x200d0701; /* a malformed prefix cannot hide a draw packet */
    assert(!nvn_command_gap_indexed_draw(altered, 12, &draw));
    assert(!nvn_command_gap_indexed_draw(packet, 11, &draw));
    assert(!nvn_command_gap_indexed_draw(NULL, 12, &draw));
    return 0;
}
