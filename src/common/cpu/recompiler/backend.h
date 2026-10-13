/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The recompiler's code generators: llvm builds LLVM IR with the LLVM-C API and writes the game module itself
   (recompiler/llvm/, the default and the recommended one); gcc goes through libgccjit (recompiler/gccjit/, optional,
   for DEC Alpha EV67 hosts) */
#ifndef SNX_RECOMPILER_BACKEND_H
#define SNX_RECOMPILER_BACKEND_H

typedef enum { SNX_BACKEND_LLVM, SNX_BACKEND_GCC } SnxBackend;

/* 0 and *backend set, or -1 for an unknown name */
int snx_backend_parse(const char* name, SnxBackend* backend);
const char* snx_backend_name(SnxBackend backend);

#endif
