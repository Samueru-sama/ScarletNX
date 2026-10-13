# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# Builds the ScarletNX runtime for Linux or FreeBSD, or for Windows with llvm-mingw (natively, or cross-built). The
# game module (<Game>_code.so or .dll) comes from `scarletnx -r`. The
# third-party libraries are fetched and built with the runtime, see cmake/dependencies.cmake.
#
#   cmake --preset default && cmake --build --preset default        (or windows-cross; see CMakePresets.json)
#   cmake -B build && cmake --build build                           (without the presets)
#
# After each build the runtime is copied next to every recompiled game in games/ (SNX_DEPLOY), so a runtime fix
# reaches the games without recompiling them.
cmake_minimum_required(VERSION 3.25)

# Clang only: the runtime, the generated game code and the third-party libraries are built and tested with one
# compiler. It is the default here, so a plain `cmake -B build` picks it over the system's cc
if(NOT DEFINED CMAKE_C_COMPILER AND NOT DEFINED ENV{CC})
    set(CMAKE_C_COMPILER clang)
endif()
if(NOT DEFINED CMAKE_CXX_COMPILER AND NOT DEFINED ENV{CXX})
    set(CMAKE_CXX_COMPILER clang++)
endif()
project(ScarletNX LANGUAGES C CXX)
if(NOT CMAKE_C_COMPILER_ID STREQUAL "Clang" OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "ScarletNX builds with Clang only, and this is ${CMAKE_C_COMPILER_ID} "
                        "(${CMAKE_C_COMPILER}). Use a preset, or -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ "
                        "in a new build folder.")
endif()

# GNU dialects: the runtime uses GNU extensions (statement expressions, __atomic builtins)
set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

option(SNX_BUILD_NX "Build the Nintendo Switch Horizon/Maxwell runtime" ON)
option(SNX_BUILD_GNUABI "Build the CPU-only Linux AArch64 runtime" ON)
# Prefer the system SDL3 on Unix. Fall back to a pinned source build if its development package is missing.
option(SNX_BUNDLED_SDL3 "Build the pinned SDL3 even when a system development package is available" OFF)
set(SNX_BACKEND sdl3 CACHE STRING "Window and input backend: sdl3 or headless")
set_property(CACHE SNX_BACKEND PROPERTY STRINGS sdl3 headless)
if(SNX_BUILD_NX AND NOT SNX_BACKEND MATCHES "^(sdl3|headless)$")
    message(FATAL_ERROR "SNX_BACKEND=${SNX_BACKEND}: the x11, x11es and win32 backends were removed, sdl3 covers them. "
                        "Configure a new build folder, or pass -DSNX_BACKEND=sdl3.")
endif()
option(SNX_VULKAN "Build the Vulkan renderer (also supports -H)" ON)
option(SNX_H264 "Decode NVDEC video (H.264) and hwopus audio (Opus) with FFmpeg" ON)
option(SNX_DEPLOY "Copy the runtime next to every recompiled game in SNX_GAMES_DIR after building" ON)
set(SNX_GAMES_DIR "${CMAKE_SOURCE_DIR}/games" CACHE PATH "Folder holding the recompiled games")

# Upstream builds with -O2. RelWithDebInfo keeps that and adds symbols for crash reports. It leaves out -DNDEBUG, as
# upstream does: the runtime has no assert(), so this only keeps the asserts of the libraries built with it (glslang,
# VMA) on, which stop at a misuse instead of carrying on with broken state
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_C_FLAGS_RELWITHDEBINFO "-O2 -g")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -g")

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
option(SNX_TOOLS_ONLY "Build the C90 extraction/recompilation driver without the runtime" OFF)
option(SNX_HACTOOL_ONLY "Build only the native container extractor" OFF)
if(SNX_BUILD_NX AND NOT SNX_TOOLS_ONLY AND NOT SNX_HACTOOL_ONLY AND NOT SNX_VULKAN)
    message(FATAL_ERROR "The NX runtime requires Vulkan. Use SNX_BACKEND=headless for a no-window build.")
endif()
set(snx_gui_default OFF)
if(SNX_BUILD_NX AND SNX_BACKEND STREQUAL "sdl3" AND NOT SNX_TOOLS_ONLY AND NOT SNX_HACTOOL_ONLY)
    set(snx_gui_default ON)
endif()
option(SNX_BUILD_GUI "Build the keyboard-driven SDL3 launcher" ${snx_gui_default})
if(SNX_BUILD_GUI AND (SNX_TOOLS_ONLY OR SNX_HACTOOL_ONLY))
    message(FATAL_ERROR "SNX_BUILD_GUI requires a runtime build, not a tools-only build")
endif()
if(SNX_HACTOOL_ONLY)
    set(SNX_H264 OFF)
    set(SNX_VULKAN OFF)
    set(SNX_BACKEND headless)
    include(cmake/dependencies.cmake)
    return()
endif()
add_subdirectory(src/common)
if(SNX_BUILD_NX)
    add_subdirectory(src/nx/runtime/driver host-tools)
endif()
include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
if(SNX_TOOLS_ONLY AND SNX_BUILD_NX)
    include(GNUInstallDirs)
    install(TARGETS scarletnx RUNTIME DESTINATION bin)
    install(FILES LICENSE LICENSE.apache NOTICE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/scarletnx)
    return()
endif()
if(SNX_BUILD_GNUABI)
    add_subdirectory(src/gnuabi)
endif()
if(SNX_BUILD_NX OR SNX_BUILD_GUI)
    include(cmake/dependencies.cmake)
endif()
if(SNX_BUILD_GUI)
    add_subdirectory(src/gui)
endif()
if(NOT SNX_BUILD_NX)
    include(GNUInstallDirs)
    install(FILES LICENSE LICENSE.apache NOTICE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/scarletnx)
    return()
endif()
add_subdirectory(src/nx)
if(SNX_VULKAN)
    add_subdirectory(tools)
endif()

# `cmake --install` puts the runtime, hactool, the desktop entry and the licenses under CMAKE_INSTALL_PREFIX, which is
# what the AppImage and the CI artifacts are made from (.github/workflows/build.yml)
include(GNUInstallDirs)
install(TARGETS scarletnx RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(TARGET hactool)
    install(TARGETS hactool RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
if(NOT WIN32)
    install(FILES dist/scarletnx.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES dist/wimsy.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
endif()

# The licenses go with the binaries: the fork's (GPL-3.0), MZ Labs' (Apache 2.0, with its NOTICE), and those of the
# libraries built into the runtime and hactool, each in a folder named after it
set(licenses "${CMAKE_INSTALL_DATADIR}/licenses/scarletnx")
install(FILES LICENSE LICENSE.apache NOTICE DESTINATION ${licenses})
if(SNX_H264)
    install(FILES "${FFmpeg_SOURCE_DIR}/COPYING.LGPLv2.1" "${FFmpeg_SOURCE_DIR}/LICENSE.md"
            DESTINATION ${licenses}/FFmpeg)
endif()
if(SNX_VULKAN)
    install(FILES "${Vulkan-Headers_SOURCE_DIR}/LICENSE.md" DESTINATION ${licenses}/Vulkan-Headers)
    install(DIRECTORY "${Vulkan-Headers_SOURCE_DIR}/LICENSES" DESTINATION ${licenses}/Vulkan-Headers)
    install(FILES "${volk_SOURCE_DIR}/LICENSE.md" DESTINATION ${licenses}/volk)
    install(FILES "${VulkanMemoryAllocator_SOURCE_DIR}/LICENSE.txt" DESTINATION ${licenses}/VulkanMemoryAllocator)
    install(FILES "${glslang_SOURCE_DIR}/LICENSE.txt" DESTINATION ${licenses}/glslang)
    install(DIRECTORY "${glslang_SOURCE_DIR}/LICENSES" DESTINATION ${licenses}/glslang)
endif()
if(SNX_SDL3_FROM_SOURCE)
    install(FILES "${SDL3_SOURCE_DIR}/LICENSE.txt" DESTINATION ${licenses}/SDL3)
endif()
if(TARGET hactool)
    install(FILES "${hactool_SOURCE_DIR}/LICENSE" DESTINATION ${licenses}/hactool)
    install(FILES "${hactool_SOURCE_DIR}/mbedtls/LICENSE" "${hactool_SOURCE_DIR}/mbedtls/apache-2.0.txt"
            DESTINATION ${licenses}/hactool/mbedtls)
    # cJSON's license is the comment at the top of its header
    file(READ "${hactool_SOURCE_DIR}/cJSON.h" cjson)
    string(FIND "${cjson}" "*/" cjson_end)
    string(SUBSTRING "${cjson}" 0 ${cjson_end} cjson)
    file(WRITE "${CMAKE_BINARY_DIR}/cJSON-LICENSE.txt" "${cjson}*/\n")
    install(FILES "${CMAKE_BINARY_DIR}/cJSON-LICENSE.txt" DESTINATION ${licenses}/hactool/cJSON RENAME LICENSE.txt)
endif()
