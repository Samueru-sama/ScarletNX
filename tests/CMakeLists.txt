# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
option(SNX_TEST_SANITIZERS "Run native regression tests with ASan and UBSan" OFF)
option(SNX_TEST_VULKAN "Run offscreen Vulkan integration tests (requires a Vulkan 1.3 ICD, e.g. lavapipe)" OFF)
if(SNX_TEST_SANITIZERS AND TARGET snx_driver)
    target_compile_options(snx_driver PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(snx_driver INTERFACE -fsanitize=address,undefined)
endif()

function(snx_test_target target standard)
    set_target_properties(${target} PROPERTIES C_STANDARD ${standard} C_STANDARD_REQUIRED ON)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)
    target_include_directories(${target} PRIVATE "${PROJECT_SOURCE_DIR}/src/common/utilities" "${PROJECT_SOURCE_DIR}/src/common/cpu/aarch64"
        "${PROJECT_SOURCE_DIR}/src/common/cpu/recompiler" "${PROJECT_SOURCE_DIR}/src/common"
        "${PROJECT_SOURCE_DIR}/src/nx" "${PROJECT_SOURCE_DIR}/src/nx/loader"
        "${PROJECT_SOURCE_DIR}/src/nx/runtime/driver")
    if(standard EQUAL 90)
        set_target_properties(${target} PROPERTIES C_EXTENSIONS OFF)
        target_compile_options(${target} PRIVATE -pedantic-errors)
    else()
        set_target_properties(${target} PROPERTIES C_EXTENSIONS ON)
    endif()
    if(NOT WIN32)
        target_compile_definitions(${target} PRIVATE _FILE_OFFSET_BITS=64)
    endif()
    target_link_libraries(${target} PRIVATE Threads::Threads)
    if(SNX_TEST_SANITIZERS)
        target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
endfunction()

# Small architectural witnesses need neither game assets nor an emulator.
add_executable(snx-test-a64-fp a64_fp_test.c "${PROJECT_SOURCE_DIR}/src/common/cpu/aarch64/cpu_helpers.c")
snx_test_target(snx-test-a64-fp 11)
if(NOT WIN32)
    target_link_libraries(snx-test-a64-fp PRIVATE m)
endif()
add_test(NAME a64_fp_witnesses COMMAND snx-test-a64-fp)

option(SNX_TEST_A64_FP_DIFF "Run the Linux AArch64 FP differential harness (requires Python unicorn 2.1.4)" OFF)
if(SNX_TEST_A64_FP_DIFF)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_CROSSCOMPILING)
        message(FATAL_ERROR "SNX_TEST_A64_FP_DIFF requires a native Linux host")
    endif()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME a64_fp_differential COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/a64_fp_diff.py" --cc "${CMAKE_C_COMPILER}")
    set_tests_properties(a64_fp_differential PROPERTIES TIMEOUT 900)
endif()

if(SNX_BUILD_NX)
foreach(backend windowed headless)
    add_executable(snx-test-startup-${backend} startup_options.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/startup.c")
    snx_test_target(snx-test-startup-${backend} 90)
    if(backend STREQUAL "headless")
        target_compile_definitions(snx-test-startup-${backend} PRIVATE SNX_HEADLESS_ONLY)
    endif()
    add_test(NAME startup_${backend} COMMAND snx-test-startup-${backend})
endforeach()

add_library(snx_test_common STATIC "${PROJECT_SOURCE_DIR}/src/common/utilities/common.c")
snx_test_target(snx_test_common 90)
if(WIN32)
    target_link_libraries(snx_test_common PUBLIC advapi32)
endif()

add_executable(snx-test-driver driver_workflow.c
    "${PROJECT_SOURCE_DIR}/src/nx/runtime/driver/driver.c" "${PROJECT_SOURCE_DIR}/src/common/utilities/process.c")
snx_test_target(snx-test-driver 90)
target_link_libraries(snx-test-driver PRIVATE snx_test_common)
add_test(NAME driver_workflow COMMAND snx-test-driver)
set_tests_properties(driver_workflow PROPERTIES TIMEOUT 60)

add_executable(snx-test-extract extract.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/driver/extract.c")
snx_test_target(snx-test-extract 90)
target_link_libraries(snx-test-extract PRIVATE snx_test_common)
add_test(NAME extraction_native COMMAND snx-test-extract)

add_executable(snx-test-loader loader_test.c "${PROJECT_SOURCE_DIR}/src/nx/loader/loader.c")
snx_test_target(snx-test-loader 90)
target_link_libraries(snx-test-loader PRIVATE snx_test_common)
add_test(NAME loader_native COMMAND snx-test-loader)

add_executable(snx-test-runtime-nro runtime_nro.c "${PROJECT_SOURCE_DIR}/src/nx/runtime/loader.c")
snx_test_target(snx-test-runtime-nro 11)
add_test(NAME runtime_nro COMMAND snx-test-runtime-nro)

add_executable(snx_recompiler_test recompiler_native.c)
snx_test_target(snx_recompiler_test 23)
target_include_directories(snx_recompiler_test PRIVATE "${PROJECT_SOURCE_DIR}/src/common/cpu/aarch64")
target_link_libraries(snx_recompiler_test PRIVATE snx_driver ${CMAKE_DL_LIBS})
add_test(NAME native_recompiler COMMAND snx_recompiler_test
    $<TARGET_FILE:scarletnx> "${PROJECT_SOURCE_DIR}" "${CMAKE_C_COMPILER}")

add_executable(snx_codegen_test codegen_test.c)
snx_test_target(snx_codegen_test 90)
target_link_libraries(snx_codegen_test PRIVATE snx_driver)
add_test(NAME native_codegen COMMAND snx_codegen_test "${CMAKE_C_COMPILER}" "${PROJECT_SOURCE_DIR}")

add_executable(snx-test-astc astc_decode.c "${PROJECT_SOURCE_DIR}/src/nx/gpu/texture/astc.c")
snx_test_target(snx-test-astc 23)
add_test(NAME astc_decode COMMAND snx-test-astc)

add_executable(snx-test-nvn-command-gap nvn_command_gap.c "${PROJECT_SOURCE_DIR}/src/nx/gpu/nvn_command_gap.c")
snx_test_target(snx-test-nvn-command-gap 23)
add_test(NAME nvn_command_gap COMMAND snx-test-nvn-command-gap)

add_executable(snx-test-audio-out audio_out.c)
snx_test_target(snx-test-audio-out 11)
target_compile_definitions(snx-test-audio-out PRIVATE SNX_NO_H264)
add_test(NAME audio_out COMMAND snx-test-audio-out)

add_executable(snx-test-fs-fixture filesystem_fixture.c)
snx_test_target(snx-test-fs-fixture 90)
target_link_libraries(snx-test-fs-fixture PRIVATE snx_test_common)

add_library(snx_test_fs_tree OBJECT "${PROJECT_SOURCE_DIR}/src/nx/hle/filesystem/fs_tree.c")
snx_test_target(snx_test_fs_tree 11)
target_compile_options(snx_test_fs_tree PRIVATE -include "${CMAKE_CURRENT_SOURCE_DIR}/fs_scan_probe.h")

foreach(suite tree service)
    add_executable(snx-test-fs-${suite} filesystem_${suite}.c $<TARGET_OBJECTS:snx_test_fs_tree>)
    snx_test_target(snx-test-fs-${suite} 11)
    add_test(NAME filesystem_${suite}
        COMMAND snx-test-fs-fixture $<TARGET_FILE:snx-test-fs-${suite}>)
endforeach()
add_test(NAME filesystem_raw
    COMMAND snx-test-fs-fixture $<TARGET_FILE:snx-test-fs-service> raw)

if(UNIX)
    add_executable(snx-test-memory memory_accounting.c
        "${PROJECT_SOURCE_DIR}/src/nx/memory/mem.c" "${PROJECT_SOURCE_DIR}/src/nx/hle/svc/svc_mem.c")
    snx_test_target(snx-test-memory 11)
    add_test(NAME memory_accounting COMMAND snx-test-memory)
    set_tests_properties(memory_accounting PROPERTIES TIMEOUT 60)
endif()

if(SNX_TEST_SANITIZERS AND WIN32)
    execute_process(COMMAND "${CMAKE_C_COMPILER}" -print-resource-dir
        OUTPUT_VARIABLE snx_clang_resource OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    set_tests_properties(a64_fp_witnesses startup_windowed startup_headless driver_workflow extraction_native loader_native runtime_nro native_recompiler native_codegen filesystem_tree filesystem_service filesystem_raw astc_decode audio_out
        PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${snx_clang_resource}/lib/windows")
endif()

endif() # SNX_BUILD_NX
include(architecture.cmake)
