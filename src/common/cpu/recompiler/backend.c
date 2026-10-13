/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "backend.h"
#include <string.h>

int snx_backend_parse(const char* name, SnxBackend* backend) {
    if (!strcmp(name, "llvm"))
        *backend = SNX_BACKEND_LLVM;
    else if (!strcmp(name, "gcc"))
        *backend = SNX_BACKEND_GCC;
    else
        return -1;
    return 0;
}

const char* snx_backend_name(SnxBackend backend) {
    switch (backend) {
        case SNX_BACKEND_GCC: return "gcc";
        default: return "llvm";
    }
}
