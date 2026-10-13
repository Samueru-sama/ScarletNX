# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# The runtime: one object library per component, linked into scarletnx. Each component sees runtime/ as its include
# root (#include "hle/kernel/kernel.h"), and the flags and third-party headers every component shares (snx_nx_flags).
#
#   core      startup, options, the game module, crash reports      kernel    threads, sync, memory, IPC
#   hle       the services the game talks to                         video     NVDEC and VIC (host1x)
#   gpu       the GPU front end: GPFIFO, engines, the renderer interface
#   shader    Maxwell shaders to GLSL                                 spirv     GLSL to SPIR-V (glslang)
#   vk        the renderer                                            platform  the OS layer and a window backend

add_library(snx_nx_flags INTERFACE)
target_include_directories(snx_nx_flags INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(snx_nx_flags INTERFACE scarletnx-common scarletnx-net)
# Every function and global used across files is declared once, in the header of the file that defines it, and
# everything else is static: a declaration that disagrees with the definition is then a compile error instead of a
# miscompile. Third-party sources keep their own warnings off.
target_compile_options(snx_nx_flags INTERFACE -fno-strict-aliasing
                       $<$<COMPILE_LANGUAGE:C>:-Werror=missing-prototypes -Werror=missing-variable-declarations>)
if(SNX_VULKAN)
    target_compile_definitions(snx_nx_flags INTERFACE SNX_VULKAN VK_NO_PROTOTYPES)
    target_link_libraries(snx_nx_flags INTERFACE Vulkan::Headers)
    target_include_directories(snx_nx_flags BEFORE INTERFACE "${volk_SOURCE_DIR}"
                                                    "${VulkanMemoryAllocator_SOURCE_DIR}/include")
endif()
if(SNX_BACKEND STREQUAL "headless")
    target_compile_definitions(snx_nx_flags INTERFACE SNX_HEADLESS_ONLY)
endif()
if(NOT SNX_H264)
    target_compile_definitions(snx_nx_flags INTERFACE SNX_NO_H264)
endif()

function(snx_component name)
    add_library(snx_${name} OBJECT ${ARGN})
    target_link_libraries(snx_${name} PUBLIC snx_nx_flags)
endfunction()

snx_component(core runtime/main.c runtime/loader.c runtime/rtapi.c runtime/clock.c runtime/crash.c runtime/options.c runtime/startup.c runtime/gdbstub.c
              runtime/present.c)
target_link_libraries(snx_core PRIVATE snx_driver)
snx_component(kernel hle/kernel/kernel.c hle/kernel/thread.c hle/kernel/sync.c hle/svc/svc.c hle/svc/svc_mem.c hle/kernel/debug.c
              memory/mem.c hle/kernel/ipc.c)
snx_component(hle hle/services/services.c hle/services/audren.c hle/services/audren_dsp.c hle/services/pl.c hle/services/sm.c hle/services/am.c hle/services/set.c hle/services/time.c hle/services/acc.c hle/services/audio.c
              hle/services/misc.c hle/services/network/bsd.c hle/services/network/dns.c
              hle/services/network/nifm.c hle/filesystem/fs.c hle/filesystem/fs_tree.c hle/services/hid.c hle/services/vi.c hle/services/nvdrv.c)
snx_component(video gpu/video/host1x.c gpu/video/nvdec.c gpu/video/vic.c)
if(SNX_H264)
    target_link_libraries(snx_video PRIVATE FFmpeg::avcodec)
    # hwopus (hle/services/audio.c) decodes Opus with FFmpeg too
    target_link_libraries(snx_hle PRIVATE FFmpeg::avcodec)
endif()
snx_component(gpu gpu/gpu.c gpu/maxwell/engines.c gpu/maxwell/mme.c gpu/maxwell/compute.c gpu/maxwell/fermi2d.c gpu/maxwell/dma.c gpu/maxwell/maxwell3d.c gpu/renderer.c
              gpu/nvn_trace.c gpu/nvn_hle.c gpu/nvn_record.c gpu/nvn_command_gap.c gpu/nvn_uniform.c gpu/nvn_pool.c gpu/nvn_buffer.c gpu/nvn_texture.c gpu/nvn_sampler.c gpu/nvn_program.c gpu/nvn_shader.c gpu/nvn_alias.c gpu/nvn_descriptor.c gpu/nvn_sampler_descriptor.c)

set(platform runtime/platform/plat.c runtime/platform/unattended.c runtime/platform/backend/${SNX_BACKEND}.c)
if(WIN32)
    list(APPEND platform runtime/platform/plat_win32.c runtime/platform/profiler_win32.c)
else()
    list(APPEND platform runtime/platform/plat_posix.c)
endif()
snx_component(platform ${platform})

set(components core kernel hle video gpu platform)
if(SNX_VULKAN)
    snx_component(shader gpu/shader/shader.c gpu/shader/decode.c gpu/shader/translate.c gpu/shader/corpus.c)
    snx_component(spirv gpu/vk/spirv.c)
    target_link_libraries(snx_spirv PUBLIC glslang::glslang glslang::glslang-default-resource-limits)
    # volk uses SDL's loader for windows and loads Vulkan directly for headless runs
    snx_component(vk gpu/vk/renderer.c gpu/vk/device.c gpu/vk/cmd.c gpu/vk/format.c gpu/vk/surface.c gpu/vk/texture.c
                  gpu/vk/nvn.c gpu/vk/buffer.c gpu/vk/writeback.c gpu/vk/pipeline.c gpu/vk/render.c gpu/vk/compute.c
                  gpu/vk/astc.c gpu/texture/etc2.c gpu/vk/vma.cpp
                  "${volk_SOURCE_DIR}/volk.c")
    list(APPEND components shader spirv vk)
    # third-party code (vma.cpp is VMA's implementation)
    set_source_files_properties("${volk_SOURCE_DIR}/volk.c" gpu/vk/vma.cpp PROPERTIES COMPILE_OPTIONS -w)
endif()

add_executable(scarletnx runtime/entry.c)
list(TRANSFORM components PREPEND snx_)
add_library(scarletnx-nx STATIC)
set_target_properties(scarletnx-nx PROPERTIES LINKER_LANGUAGE CXX)
target_link_libraries(scarletnx-nx PUBLIC snx_nx_flags ${components} snx_driver)
target_link_libraries(scarletnx PRIVATE scarletnx-nx)
# glslang is C++: link with the C++ driver. It goes in the build folder itself, where
# the native driver and CI look for it
set_target_properties(scarletnx PROPERTIES LINKER_LANGUAGE CXX RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
if(WIN32)
    # Static, so the runtime runs without the MinGW DLLs next to it. user32 has the backends' message boxes:
    # winmm is loaded at run time, and the static SDL3 brings the libraries it needs
    target_link_libraries(scarletnx PRIVATE user32)
    # The application manifest (runtime/platform/runtime.manifest): UTF-8 as the process's code page, long paths
    enable_language(RC)
    target_sources(scarletnx PRIVATE runtime/platform/runtime.rc)
    target_link_options(scarletnx PRIVATE -static)
    if(SNX_BACKEND STREQUAL "sdl3")
        target_link_options(scarletnx PRIVATE -mwindows)
    else()
        target_link_options(scarletnx PRIVATE -mconsole)
    endif()
else()
    # No -rdynamic: the game module gets the runtime through RtAPI and imports nothing from it, and exporting every
    # global would let a library loaded later (a GPU driver) bind its own symbols to ours by name
    target_link_libraries(scarletnx PRIVATE Threads::Threads ${CMAKE_DL_LIBS} m)
endif()

# SDL3 is either the system package or the pinned static source build (cmake/dependencies.cmake)
if(SNX_BACKEND STREQUAL "sdl3")
    target_link_libraries(snx_platform PRIVATE SDL3::SDL3)
    target_link_libraries(scarletnx PRIVATE SDL3::SDL3)
endif()

# The shader translator offline: F2I's negate and absolute value bits
if(BUILD_TESTING AND SNX_VULKAN)
    add_executable(snx-test-shader-f2i "${PROJECT_SOURCE_DIR}/tests/shader_f2i.c")
    target_link_libraries(snx-test-shader-f2i PRIVATE snx_shader)
    if(NOT WIN32)
        target_link_libraries(snx-test-shader-f2i PRIVATE Threads::Threads m)
    endif()
    add_test(NAME shader_f2i COMMAND snx-test-shader-f2i)
endif()

# The shader translator and its SPIR-V step offline (no ICD or GPU): the FP32 denormal mode a shader declares
if(BUILD_TESTING AND SNX_VULKAN)
    add_executable(snx-test-shader-denorm "${PROJECT_SOURCE_DIR}/tests/shader_denorm.c")
    target_link_libraries(snx-test-shader-denorm PRIVATE snx_shader snx_spirv)
    if(NOT WIN32)
        target_link_libraries(snx-test-shader-denorm PRIVATE Threads::Threads m)
    endif()
    set_target_properties(snx-test-shader-denorm PROPERTIES LINKER_LANGUAGE CXX)
    add_test(NAME shader_denorm COMMAND snx-test-shader-denorm)
endif()

# The shader translator and glslang offline (no ICD or GPU): the float math keeps the console's order
if(BUILD_TESTING AND SNX_VULKAN)
    add_executable(snx-test-shader-precise "${PROJECT_SOURCE_DIR}/tests/shader_precise.c")
    target_link_libraries(snx-test-shader-precise PRIVATE snx_shader snx_spirv)
    if(NOT WIN32)
        target_link_libraries(snx-test-shader-precise PRIVATE Threads::Threads m)
    endif()
    set_target_properties(snx-test-shader-precise PROPERTIES LINKER_LANGUAGE CXX)
    add_test(NAME shader_precise COMMAND snx-test-shader-precise)
endif()

# The shader translator offline: which c0 words become storage buffers
if(BUILD_TESTING AND SNX_VULKAN)
    add_executable(snx-test-shader-gbuf "${PROJECT_SOURCE_DIR}/tests/shader_gbuf.c")
    target_link_libraries(snx-test-shader-gbuf PRIVATE snx_shader)
    if(NOT WIN32)
        target_link_libraries(snx-test-shader-gbuf PRIVATE Threads::Threads m)
    endif()
    add_test(NAME shader_gbuf COMMAND snx-test-shader-gbuf)
endif()

# Opt-in because normal unit-test runs need neither an ICD nor a GPU. All rendering uses the real runtime.
if(BUILD_TESTING AND SNX_TEST_VULKAN AND SNX_VULKAN)
    add_executable(snx-test-vulkan "${PROJECT_SOURCE_DIR}/tests/vulkan_headless.c")
    target_link_libraries(snx-test-vulkan PRIVATE snx_nx_flags ${components} snx_driver Threads::Threads ${CMAKE_DL_LIBS})
    set_target_properties(snx-test-vulkan PROPERTIES LINKER_LANGUAGE CXX)
    if(WIN32)
        target_link_libraries(snx-test-vulkan PRIVATE user32)
    else()
        target_link_libraries(snx-test-vulkan PRIVATE m)
    endif()
    if(SNX_BACKEND STREQUAL "sdl3")
        target_link_libraries(snx-test-vulkan PRIVATE SDL3::SDL3)
    endif()
    add_test(NAME vulkan_headless COMMAND snx-test-vulkan -H)
    set_tests_properties(vulkan_headless PROPERTIES TIMEOUT 60
        FAIL_REGULAR_EXPRESSION "vulkan validation:;failed: VK_ERROR")
endif()

if(SNX_DEPLOY)
    add_custom_command(TARGET scarletnx POST_BUILD
        COMMAND "${CMAKE_COMMAND}" "-DRUNTIME=$<TARGET_FILE:scarletnx>" "-DGAMES=${SNX_GAMES_DIR}" "-DWINDOWS=${WIN32}"
                -P "${CMAKE_SOURCE_DIR}/cmake/deploy.cmake"
        VERBATIM)
endif()

# All model names share the same Horizon/Maxwell implementation.
foreach(model erista mariko aula)
    if(WIN32)
        add_custom_command(TARGET scarletnx POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:scarletnx>"
                    "${CMAKE_BINARY_DIR}/scarletnx-${model}.exe" VERBATIM)
        install(PROGRAMS "${CMAKE_BINARY_DIR}/scarletnx-${model}.exe" DESTINATION bin)
    else()
        add_custom_command(TARGET scarletnx POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E create_symlink "$<TARGET_FILE_NAME:scarletnx>"
                    "${CMAKE_BINARY_DIR}/scarletnx-${model}" VERBATIM)
        install(PROGRAMS "${CMAKE_BINARY_DIR}/scarletnx-${model}" DESTINATION bin)
    endif()
endforeach()
