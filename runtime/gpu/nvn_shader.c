/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gpu/nvn_shader.h"
#include "gpu/gpu.h"
#include "gpu/shader/shader.h"
#include <string.h>

int nvn_shader_load(u64 address, u8 code[NVN_SHADER_CODE_MAX], NvnShaderMeta* out) {
    if (!address || address > UINT64_MAX - 0x30 - NVN_SHADER_CODE_MAX) return 0;
    u32 header[4] = {0};
    gmem_read_code(address, header, sizeof header);
    if (header[0] != 0x12345678 || header[1] < 0x80 || header[1] > NVN_SHADER_CODE_MAX + 0x30) return 0;
    /* Catherine and TotK put NVN's 0x30-byte blob header before the 80-byte Maxwell SPH. */
    gmem_read_code(address + 0x30, code, NVN_SHADER_CODE_MAX);
    u32 sph;
    memcpy(&sph, code, sizeof sph);
    u32 kind = sph & 31;
    u32 type = (sph >> 10) & 15;
    int stage = -1;
    /* NVN's vertex-A/vertex-B headers use type 0/1; the Maxwell SPH uses type 5 for pixels.
       Leave tessellation, geometry, and compute unknown until their NVN code layouts are observed. */
    if (kind == 1 && type <= 1)
        stage = ST_VS;
    else if (kind == 2 && type == 5)
        stage = ST_FS;
    *out = (NvnShaderMeta){.blob_size = header[1], .sph_word = sph, .stage = stage};
    return 1;
}
