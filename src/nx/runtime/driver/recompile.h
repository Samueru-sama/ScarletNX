/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_RECOMPILE_H
#define SNX_RECOMPILE_H
#include "common.h"
/* the ABI stamped into the games this build recompiles */
unsigned snx_game_abi(void);
int snx_recompile(int argc, char** argv, const SnxPaths* paths);
#endif
