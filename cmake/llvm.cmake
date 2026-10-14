# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# LLVM's C API is required by the recompiler on every platform. SNX_LLVM_CONFIG selects an llvm-config that runs
# on the build machine and describes the LLVM development files for the target, including when cross-compiling.
set(snx_llvm_names llvm-config)
foreach(version RANGE 30 17 -1)
    list(APPEND snx_llvm_names llvm-config-${version} llvm-config${version})
endforeach()
find_program(SNX_LLVM_CONFIG NAMES ${snx_llvm_names} DOC "llvm-config of the LLVM the LLVM backend links")
if(NOT SNX_LLVM_CONFIG)
    message(FATAL_ERROR "LLVM's development files are required by the recompiler on every platform. Install them "
                        "or point SNX_LLVM_CONFIG at an llvm-config for the target LLVM installation.")
endif()

function(snx_llvm_query output)
    execute_process(COMMAND "${SNX_LLVM_CONFIG}" ${ARGN} OUTPUT_VARIABLE value
        OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE result ERROR_VARIABLE error)
    if(NOT "${result}" STREQUAL "0")
        message(FATAL_ERROR "LLVM is required, but ${SNX_LLVM_CONFIG} ${ARGN} failed (${result}): ${error}")
    endif()
    set(${output} "${value}" PARENT_SCOPE)
endfunction()

snx_llvm_query(SNX_LLVM_VERSION --version)
snx_llvm_query(SNX_LLVM_INCLUDE_DIR --includedir)
snx_llvm_query(SNX_LLVM_LIBRARY_DIR --libdir)
snx_llvm_query(SNX_LLVM_LIBS --libs core native passes target analysis)
snx_llvm_query(SNX_LLVM_SYSTEM_LIBS --system-libs core native passes target analysis)
if(NOT SNX_LLVM_VERSION OR NOT EXISTS "${SNX_LLVM_INCLUDE_DIR}/llvm-c/Core.h" OR
   NOT IS_DIRECTORY "${SNX_LLVM_LIBRARY_DIR}" OR NOT SNX_LLVM_LIBS)
    message(FATAL_ERROR "LLVM's development files are required, but ${SNX_LLVM_CONFIG} reports an incomplete "
                        "installation. Check its headers and libraries or select another SNX_LLVM_CONFIG.")
endif()
separate_arguments(SNX_LLVM_LIBS UNIX_COMMAND "${SNX_LLVM_LIBS}")
separate_arguments(SNX_LLVM_SYSTEM_LIBS UNIX_COMMAND "${SNX_LLVM_SYSTEM_LIBS}")
message(STATUS "LLVM backend: LLVM ${SNX_LLVM_VERSION} (${SNX_LLVM_CONFIG})")
