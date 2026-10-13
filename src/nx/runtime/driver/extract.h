/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_EXTRACT_H
#define SNX_EXTRACT_H

#include "common.h"

/* argv contains extraction arguments only, without a program name or --extract. */
int snx_extract(int argc, char** argv, const SnxPaths* paths);

#endif
