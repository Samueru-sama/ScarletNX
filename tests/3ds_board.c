/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "board/board.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    SnxCtrBoard board = {0};
    assert(!snx_ctr_board_reset(&board));
    assert(!snx_ctr_board_advance(&board, 1));
    assert(!snx_ctr_board_init(NULL, SNX_CTR_OLD));
    assert(!snx_ctr_profile((SnxCtrModel)-1));
    assert(!snx_ctr_physical_region(NULL, 0, 1));
    for (unsigned m = 0; m < 2; ++m) {
        assert(snx_ctr_board_init(&board, (SnxCtrModel)m));
        const SnxCtrProfile* p = board.profile;
        assert(board.powered && !board.ticks && p->model == (SnxCtrModel)m);
        assert(p->arm11_cores == (m ? 4u : 2u));
        assert(p->fcram_size == (m ? 0x10000000u : 0x08000000u));
        for (size_t i = 0; i < p->region_count; ++i) {
            const SnxCtrRegion* r = &p->regions[i];
            assert(r->size && (uint64_t)r->base + r->size <= UINT64_C(0x100000000));
            if (i) assert((uint64_t)p->regions[i - 1].base + p->regions[i - 1].size <= r->base);
            assert(snx_ctr_physical_region(p, r->base, r->size) == r);
            assert(snx_ctr_physical_region(p, r->base + r->size - 1, 1) == r);
            assert(!snx_ctr_physical_region(p, r->base + r->size - 1, 2));
            assert(!snx_ctr_physical_region(p, r->base, 0));
        }
        assert(!snx_ctr_physical_region(p, 0x00100000u, 4)); /* process VA is not PA */
        assert(!snx_ctr_physical_region(p, 0xfffffffcu, 8));
        assert(!snx_ctr_physical_region(p, 0x20000001u, UINT32_MAX));
        assert(!!snx_ctr_physical_region(p, 0x1f000000u, 4) == !!m);
        assert(!!snx_ctr_physical_region(p, 0x28000000u, 4) == !!m);
        assert(snx_ctr_physical_region(p, 0x10100000u, 4)->kind == SNX_CTR_MMIO);
        assert(snx_ctr_physical_region(p, 0x18000000u, 4)->kind == SNX_CTR_VRAM);
        assert(snx_ctr_board_advance(&board, SNX_CTR_TICKS_PER_FRAME));
        assert(board.ticks == SNX_CTR_TICKS_PER_FRAME);
        assert(!snx_ctr_board_advance(&board, UINT64_MAX));
        assert(board.ticks == SNX_CTR_TICKS_PER_FRAME);
        assert(snx_ctr_board_reset(&board) && !board.ticks && board.profile == p);
        assert(snx_ctr_board_advance(&board, UINT64_MAX));
        assert(!snx_ctr_board_advance(&board, 1) && board.ticks == UINT64_MAX);
        snx_ctr_board_shutdown(&board);
        assert(!board.powered && !board.profile && !board.ticks);
        assert(!snx_ctr_board_advance(&board, 0) && !snx_ctr_board_reset(&board));
    }
    assert(!snx_ctr_board_init(&board, (SnxCtrModel)99));
    assert(!board.powered && !board.profile);
    snx_ctr_board_shutdown(NULL);
    puts("3DS board profiles, physical bounds, lifecycle and base timing passed");
    return 0;
}
