# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# The LLVM backend and the runtime library it links into every module (rtlib.c): helpers and interpreter, compiled
# here with this Clang and the flags toolchain.c gives generated C (no -march=native: it ships in the recompiler)
set(snx_cpu "${PROJECT_SOURCE_DIR}/src/common/cpu/aarch64")
add_executable(snx-rtlib-gen rtlib_gen.c)
target_link_libraries(snx-rtlib-gen PRIVATE scarletnx-common)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/rtlib_interp.inc"
    COMMAND snx-rtlib-gen interp "${CMAKE_CURRENT_BINARY_DIR}/rtlib_interp.inc"
    DEPENDS snx-rtlib-gen VERBATIM)
set(snx_rtlib_flags -O2 -fvisibility=hidden -fno-strict-aliasing -fwrapv -w -mcx16 -fno-semantic-interposition
    -foptimize-sibling-calls -fno-stack-protector)
if(WIN32)
    list(APPEND snx_rtlib_flags -mno-stack-arg-probe)
else()
    list(APPEND snx_rtlib_flags -fPIC)
endif()
file(GLOB snx_rtlib_sources CONFIGURE_DEPENDS "${snx_cpu}/*.h" "${snx_cpu}/*.c" "${snx_cpu}/helpers/*.c"
    "${PROJECT_SOURCE_DIR}/src/common/runtime/interface.h")
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/rtlib${CMAKE_C_OUTPUT_EXTENSION}"
    COMMAND "${CMAKE_C_COMPILER}" ${snx_rtlib_flags} -I "${CMAKE_CURRENT_BINARY_DIR}" -I "${snx_cpu}"
        -I "${PROJECT_SOURCE_DIR}/src/common" -c "${CMAKE_CURRENT_SOURCE_DIR}/rtlib.c"
        -o "${CMAKE_CURRENT_BINARY_DIR}/rtlib${CMAKE_C_OUTPUT_EXTENSION}"
    DEPENDS rtlib.c "${CMAKE_CURRENT_BINARY_DIR}/rtlib_interp.inc" ${snx_rtlib_sources} VERBATIM)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/rtlib_object.c"
    COMMAND snx-rtlib-gen embed "${CMAKE_CURRENT_BINARY_DIR}/rtlib${CMAKE_C_OUTPUT_EXTENSION}"
        "${CMAKE_CURRENT_BINARY_DIR}/rtlib_object.c" snx_rtlib_object
    DEPENDS snx-rtlib-gen "${CMAKE_CURRENT_BINARY_DIR}/rtlib${CMAKE_C_OUTPUT_EXTENSION}" VERBATIM)

add_library(snx_llvm STATIC ir.c ir_a64.c ir_simd.c ir_fp.c ir_codegen.c link.c link_elf.c link_coff.c
    "${CMAKE_CURRENT_BINARY_DIR}/rtlib_object.c")
set_target_properties(snx_llvm PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS ON)
target_include_directories(snx_llvm PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(snx_llvm SYSTEM PUBLIC "${SNX_LLVM_INCLUDE_DIR}")
target_compile_options(snx_llvm PRIVATE -Wall -Wextra -Werror=missing-prototypes)
target_compile_definitions(snx_llvm PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_directories(snx_llvm PUBLIC "${SNX_LLVM_LIBRARY_DIR}")
target_link_libraries(snx_llvm PUBLIC scarletnx-common ${SNX_LLVM_LIBS} ${SNX_LLVM_SYSTEM_LIBS} Threads::Threads)
