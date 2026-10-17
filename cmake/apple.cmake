# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
#
# For a macOS or iOS build: SNX_APPLE_SDK, its SDK's path, and SNX_APPLE_FLAGS, its architecture, SDK and oldest system
# version (CMAKE_OSX_ARCHITECTURES, CMAKE_OSX_SYSROOT, CMAKE_OSX_DEPLOYMENT_TARGET) as compiler flags, for what is
# compiled outside CMake's own compiles: FFmpeg's and OpenSSL's builds and the recompiler's runtime library. CMake adds
# them to its compiles itself. Both are empty elsewhere. On iOS without them those would build for the Mac doing the
# build.
include_guard(GLOBAL)
set(SNX_APPLE_FLAGS "")
set(SNX_APPLE_SDK "")
if(APPLE)
    list(LENGTH CMAKE_OSX_ARCHITECTURES snx_apple_arch_count)
    if(snx_apple_arch_count GREATER 1)
        message(FATAL_ERROR "One architecture at a time: FFmpeg and OpenSSL build for one (CMAKE_OSX_ARCHITECTURES is "
                            "${CMAKE_OSX_ARCHITECTURES}).")
    endif()
    if(CMAKE_OSX_ARCHITECTURES)
        list(APPEND SNX_APPLE_FLAGS -arch ${CMAKE_OSX_ARCHITECTURES})
    endif()
    # an SDK named (iphoneos), or none (the Mac's), is a path to the compilers outside CMake
    set(SNX_APPLE_SDK "${CMAKE_OSX_SYSROOT}")
    if(NOT SNX_APPLE_SDK)
        set(SNX_APPLE_SDK macosx)
    endif()
    if(NOT IS_ABSOLUTE "${SNX_APPLE_SDK}")
        execute_process(COMMAND xcrun --sdk "${SNX_APPLE_SDK}" --show-sdk-path OUTPUT_VARIABLE snx_apple_path
                        OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE snx_apple_result)
        if(NOT snx_apple_result EQUAL 0 OR NOT IS_DIRECTORY "${snx_apple_path}")
            message(FATAL_ERROR "xcrun found no SDK named ${SNX_APPLE_SDK}: install Xcode with its platform, or "
                                "set CMAKE_OSX_SYSROOT to the SDK's path.")
        endif()
        set(SNX_APPLE_SDK "${snx_apple_path}")
    endif()
    list(APPEND SNX_APPLE_FLAGS -isysroot "${SNX_APPLE_SDK}")
    if(CMAKE_OSX_DEPLOYMENT_TARGET AND IOS)
        list(APPEND SNX_APPLE_FLAGS -miphoneos-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET})
    elseif(CMAKE_OSX_DEPLOYMENT_TARGET)
        list(APPEND SNX_APPLE_FLAGS -mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET})
    endif()
endif()
