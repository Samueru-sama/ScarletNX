/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_SPIRV_EMITTER_FIXTURE_H
#define SNX_SPIRV_EMITTER_FIXTURE_H
#include <stddef.h>
#include <stdint.h>

/* Compute shader: one invocation writes 40 + 2 to set 0, binding 0, word 0. */
uint32_t* snx_test_spirv_compute(size_t* words);
/* Minimal graphics stages for checking Vulkan's entry point and output rules. */
uint32_t* snx_test_spirv_graphics(int fragment, size_t* words);
#endif
