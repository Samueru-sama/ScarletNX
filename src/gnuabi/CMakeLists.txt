# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
add_library(scarletnx-gnuabi STATIC
    abi/syscalls/syscalls.c loader/elf/elf.c memory/memory.c process/process.c
    signals/signals.c threading/futex.c runtime/runtime.c runtime/execution.c runtime/arm_exec.c
    filesystem/vfs.c filesystem/devfs/devfs.c filesystem/tmpfs/tmpfs.c filesystem/procfs/procfs.c
    network/socket.c network/poll.c network/etc.c)
set_target_properties(scarletnx-gnuabi PROPERTIES OUTPUT_NAME scarletnx-gnuabi-runtime C_STANDARD 23 C_STANDARD_REQUIRED ON)
target_include_directories(scarletnx-gnuabi PUBLIC "${PROJECT_SOURCE_DIR}/src/gnuabi" "${PROJECT_SOURCE_DIR}/src/common")
# AArch32 execution needs the ARM LLVM block backend. Build it here when the NX
# runtime (which normally owns it) is not part of this configuration.
if(NOT TARGET snx_arm_llvm AND NOT SNX_BUILD_NX)
    include("${CMAKE_SOURCE_DIR}/cmake/llvm.cmake")
    add_subdirectory("${CMAKE_SOURCE_DIR}/src/common/cpu/arm" gnuabi-arm-backend)
endif()
target_link_libraries(scarletnx-gnuabi PUBLIC scarletnx-common scarletnx-net snx_arm_llvm)
target_compile_options(scarletnx-gnuabi PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(scarletnx-gnuabi PRIVATE _CRT_SECURE_NO_WARNINGS)
if(WIN32)
    target_link_libraries(scarletnx-gnuabi PRIVATE bcrypt)
endif()
add_executable(scarletnx-gnuabi-run runtime/main.c)
target_link_libraries(scarletnx-gnuabi-run PRIVATE scarletnx-gnuabi)
set_target_properties(scarletnx-gnuabi-run PROPERTIES OUTPUT_NAME scarletnx-gnuabi
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}" C_STANDARD 23 C_STANDARD_REQUIRED ON)
if(BUILD_TESTING)
    add_executable(snx-test-gnuabi-runtime "${PROJECT_SOURCE_DIR}/tests/gnuabi_runtime.c")
    target_link_libraries(snx-test-gnuabi-runtime PRIVATE scarletnx-gnuabi)
    target_compile_options(snx-test-gnuabi-runtime PRIVATE -Wall -Wextra -Werror)
    set_target_properties(snx-test-gnuabi-runtime PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON)
    add_test(NAME gnuabi_memory_elf_syscalls COMMAND snx-test-gnuabi-runtime)
    add_executable(snx-test-gnuabi-vfs "${PROJECT_SOURCE_DIR}/tests/gnuabi_vfs.c")
    target_link_libraries(snx-test-gnuabi-vfs PRIVATE scarletnx-gnuabi)
    target_compile_options(snx-test-gnuabi-vfs PRIVATE -Wall -Wextra -Werror)
    set_target_properties(snx-test-gnuabi-vfs PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON)
    add_test(NAME gnuabi_vfs_isolation COMMAND snx-test-gnuabi-vfs)
    add_executable(snx-test-gnuabi-network "${PROJECT_SOURCE_DIR}/tests/gnuabi_network.c")
    target_link_libraries(snx-test-gnuabi-network PRIVATE scarletnx-gnuabi)
    target_compile_options(snx-test-gnuabi-network PRIVATE -Wall -Wextra -Werror)
    set_target_properties(snx-test-gnuabi-network PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON)
    add_test(NAME gnuabi_network COMMAND snx-test-gnuabi-network)
    set_tests_properties(gnuabi_network PROPERTIES TIMEOUT 30)
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME gnuabi_elf32 COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/gnuabi_elf32.py"
            $<TARGET_FILE:scarletnx-gnuabi-run>)
    endif()
endif()

include(GNUInstallDirs)
install(TARGETS scarletnx-gnuabi-run RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
