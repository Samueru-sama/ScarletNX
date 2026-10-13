# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
add_library(snx_driver STATIC driver.c extract.c recompile.c toolchain.c
    ../../loader/loader.c ../../loader/module.c ../../loader/codegen_view.c)
set_target_properties(snx_driver PROPERTIES C_STANDARD 90 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_include_directories(snx_driver PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../../loader")
target_compile_definitions(snx_driver PRIVATE
    SNX_SOURCE_DIR="${CMAKE_SOURCE_DIR}" SNX_BUILD_DIR="${CMAKE_BINARY_DIR}" _CRT_SECURE_NO_WARNINGS
    SNX_NX_ABI_INCLUDE="${CMAKE_SOURCE_DIR}/src/nx/loader"
    SNX_COMMON_INCLUDE="${CMAKE_SOURCE_DIR}/src/common")
target_compile_options(snx_driver PRIVATE -pedantic-errors -Wall -Wextra -Werror=missing-prototypes)
target_link_libraries(snx_driver PUBLIC scarletnx-common Threads::Threads)
if(WIN32)
    target_link_libraries(snx_driver PUBLIC advapi32)
endif()
if(NOT SNX_TOOLS_ONLY)
    target_compile_definitions(snx_driver PRIVATE SNX_WITH_RUNTIME)
endif()
if(SNX_TOOLS_ONLY)
    add_executable(scarletnx main.c)
    set_target_properties(scarletnx PROPERTIES C_STANDARD 90 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    target_link_libraries(scarletnx PRIVATE snx_driver)
    if(WIN32)
        if(CMAKE_C_SIMULATE_ID STREQUAL "MSVC")
            target_link_options(scarletnx PRIVATE "LINKER:/MANIFESTINPUT:${CMAKE_SOURCE_DIR}/src/nx/runtime/platform/runtime.manifest")
        else()
            enable_language(RC)
            target_sources(scarletnx PRIVATE "${CMAKE_SOURCE_DIR}/src/nx/runtime/platform/runtime.rc")
            target_include_directories(scarletnx PRIVATE "${CMAKE_SOURCE_DIR}/src/nx/runtime/platform")
        endif()
    endif()
endif()
