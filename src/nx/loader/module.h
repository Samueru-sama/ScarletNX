/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Derived from SwitchRecomp module.py, Copyright 2026 MZ Labs, Apache-2.0.
   Retain LICENSE.apache and NOTICE; the fork's changes are GPL-3.0-or-later. */
#ifndef SNX_TOOL_MODULE_H
#define SNX_TOOL_MODULE_H

#include "loader.h"

typedef struct {
    const char *outdir, *title;
    size_t chunk_instrs;
    int interp_only, quiet, hook_direct_calls;
    const SnxU64* hints;
    size_t hint_count;
    unsigned abi;
} SnxCodegenOptions;

typedef struct {
    char** paths;
    size_t count;
} SnxSources;

/* Returns 0 on success, nonzero on output failure. outdir must already exist. */
int snx_generate_module(SnxProgram* loaded, const SnxCodegenOptions* options, SnxSources* sources);
void snx_sources_free(SnxSources* sources);

#endif
