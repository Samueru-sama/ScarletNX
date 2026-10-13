# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
add_library(snx_jit STATIC jit.c jit_a64.c jit_fp.c jit_simd.c)
set_target_properties(snx_jit PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS ON)
target_include_directories(snx_jit PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(snx_jit SYSTEM PUBLIC "${SNX_GCCJIT_INCLUDE_DIR}")
target_compile_options(snx_jit PRIVATE -Wall -Wextra -Werror=missing-prototypes)
target_link_libraries(snx_jit PUBLIC scarletnx-common "${SNX_GCCJIT_LIBRARY}")
