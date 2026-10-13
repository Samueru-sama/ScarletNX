# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: Copyright 2026 ScarletNX contributors
cmake_minimum_required(VERSION 3.20)
project(snx_shader_differential LANGUAGES C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 23)
if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang" OR NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "This Linux harness requires Clang for C and C++")
endif()
set(EDEN_DIR "" CACHE PATH "Pinned Eden checkout")
set(DEPENDENCY_INCLUDE "" CACHE PATH "Directory containing boost/ and fmt/")
set(SCARLET_SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../src/nx/gpu/shader" CACHE PATH "Override for baseline replay")
if(NOT EXISTS "${EDEN_DIR}/src/shader_recompiler/frontend/maxwell/maxwell.inc")
    message(FATAL_ERROR "Set EDEN_DIR to Eden 6352612425ae0765802eaa8c5904ff652b97222a")
endif()
add_executable(scarlet_frontend scarlet_frontend.c
    ${SCARLET_SHADER_DIR}/shader.c ${SCARLET_SHADER_DIR}/translate.c ${SCARLET_SHADER_DIR}/decode.c)
target_include_directories(scarlet_frontend PRIVATE "${SCARLET_SHADER_DIR}/../.." ../../src/common)
target_compile_options(scarlet_frontend PRIVATE -Wall -Wextra -Werror -Wno-missing-field-initializers)

set(IR ${EDEN_DIR}/src/shader_recompiler/frontend/ir)
set(MW ${EDEN_DIR}/src/shader_recompiler/frontend/maxwell)
set(IMPL ${MW}/translate/impl)
add_executable(eden_frontend eden_frontend.cpp
    ${IR}/basic_block.cpp ${IR}/condition.cpp ${IR}/ir_emitter.cpp
    ${IR}/microinstruction.cpp ${IR}/opcodes.cpp ${IR}/patch.cpp ${IR}/type.cpp ${IR}/value.cpp
    ${MW}/decode.cpp ${MW}/opcodes.cpp
    ${IMPL}/impl.cpp ${IMPL}/common_funcs.cpp
    ${IMPL}/move_register.cpp ${IMPL}/integer_to_integer_conversion.cpp
    ${IMPL}/integer_funnel_shift.cpp ${IMPL}/bitfield_insert.cpp
    ${IMPL}/logic_operation.cpp ${IMPL}/move_predicate_to_register.cpp
    ${IMPL}/predicate_set_predicate.cpp ${IMPL}/predicate_set_register.cpp
    ${IMPL}/condition_code_set.cpp ${IMPL}/integer_set_predicate.cpp
    ${IMPL}/integer_compare_and_set.cpp ${IMPL}/floating_point_set_predicate.cpp
    ${IMPL}/floating_point_compare_and_set.cpp ${IMPL}/floating_point_conversion_integer.cpp
    ${IMPL}/half_floating_point_set_predicate.cpp ${IMPL}/half_floating_point_helper.cpp)
target_include_directories(eden_frontend PRIVATE ${EDEN_DIR}/src ${EDEN_DIR}/tools ${DEPENDENCY_INCLUDE})
target_compile_definitions(eden_frontend PRIVATE FMT_HEADER_ONLY)
target_compile_options(eden_frontend PRIVATE -fbracket-depth=1024)
option(ENABLE_UBSAN "Instrument the harness and both frontends" ON)
if(ENABLE_UBSAN)
    foreach(target scarlet_frontend eden_frontend)
        target_compile_options(${target} PRIVATE -fsanitize=undefined -fno-sanitize-recover=all)
        target_link_options(${target} PRIVATE -fsanitize=undefined)
    endforeach()
endif()
