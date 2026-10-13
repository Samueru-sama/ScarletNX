/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* GLSL to SPIR-V for the Vulkan renderer (glslang) */
#ifndef SNX_VK_SPIRV_H
#define SNX_VK_SPIRV_H
#include <stddef.h>
#include "gpu/shader/shader.h"

/* the translator's GLSL patched for the renderer: the alpha test and the depth range through push constants. Takes
   and returns malloc'd text (NULL stays NULL) */
char* vk_glsl_post(char* src, int stage, int flush_denorms);
/* the shaders declare that infinities, NaNs and signed zeros are kept (SPV_KHR_float_controls), where the device
   can: 0 when it can't (gpu/vk/device.c). On by default, for snx-shaderc */
extern int g_vk_preserve_infnan;
extern int g_vk_flush_denorm; /* the device can flush FP32 denormals on their own (DenormFlushToZero, 32 bit) */
/* malloc'd SPIR-V words, or NULL with the compiler's log in err, through an on-disk cache: cache_dir/<hash>.spv, the
   hash of the GLSL text, the stage, glslang's version and the compile target, so a new compiler compiles again.
   cache_dir NULL compiles without one */
u32* vk_spirv_cached(const char* glsl, int stage, const char* cache_dir, size_t* nwords, char* err, size_t errn);

#endif
