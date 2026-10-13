# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
add_library(scarletnx-common STATIC
    utilities/common.c utilities/process.c
    cpu/aarch64/a64.c cpu/recompiler/codegen.c cpu/execution/execution.c)
target_include_directories(scarletnx-common PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/utilities"
    "${CMAKE_CURRENT_SOURCE_DIR}/cpu/aarch64" "${CMAKE_CURRENT_SOURCE_DIR}/cpu/recompiler")
target_compile_definitions(scarletnx-common PRIVATE _CRT_SECURE_NO_WARNINGS
    SNX_HOST_COMPILER="${CMAKE_C_COMPILER}"
    SNX_CPU_INCLUDE="${CMAKE_CURRENT_SOURCE_DIR}/cpu/aarch64"
    SNX_COMMON_INCLUDE="${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(scarletnx-common PRIVATE -fno-strict-aliasing -Wall -Wextra -Werror=missing-prototypes)
target_link_libraries(scarletnx-common PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
if(WIN32)
    target_link_libraries(scarletnx-common PUBLIC advapi32)
else()
    target_link_libraries(scarletnx-common PUBLIC m)
endif()
