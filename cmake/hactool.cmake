# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# hactool (target hactool), used by scarletnx --extract, with the mbedtls copy it bundles. Its
# Makefile builds mbedtls with mbedtls's own Makefile, whose CMake script is too old for CMake 4, so both are built
# from their sources here. hactool only uses mbedtls's crypto, and the linker leaves the rest of the library out.
set(hactool_src "${hactool_SOURCE_DIR}")
# CPM applied cmake/hactool-decrypted.patch when it unpacked hactool. patch(1) also applies a hunk whose context moved
# or changed (offset, fuzz) without an error, and a patch that failed part way leaves a copy the next configure reuses,
# so the patched files have to match the hashes they had when the patch was written
set(hactool_patched_nca 7902a46ade61e9801a2a0ad1e98993135e39684ba64dbcbaebccad4d56c7e315)
set(hactool_patched_xci c0ce01f8b433d28979a6f655377b21192d1909996bd625973b1eb28818b56d2e)
foreach(name nca xci)
    file(SHA256 "${hactool_src}/${name}.c" hactool_have)
    if(NOT hactool_have STREQUAL "${hactool_patched_${name}}")
        message(FATAL_ERROR "${hactool_src}/${name}.c (SHA-256 ${hactool_have}) is not what "
                            "cmake/hactool-decrypted.patch makes of the pinned hactool. A patch that failed part way "
                            "leaves its copy in the CPM cache: delete that folder and configure again to see the "
                            "error. After a hactool bump or a patch edit, check the patched files and update the "
                            "hashes in cmake/hactool.cmake")
    endif()
endforeach()
file(GLOB mbedtls_sources CONFIGURE_DEPENDS "${hactool_src}/mbedtls/library/*.c")
add_library(hactool_mbedtls STATIC ${mbedtls_sources})
# A plain -I, not -isystem: the compiler searches every -I folder before any -isystem one, so with -isystem an
# -I/usr/local/include in CFLAGS (usual on FreeBSD) put a system mbedtls 3 ahead of the bundled 2.x headers
target_include_directories(hactool_mbedtls BEFORE PUBLIC "${hactool_src}/mbedtls/include")
target_compile_options(hactool_mbedtls PRIVATE -w)
set_target_properties(hactool_mbedtls PROPERTIES C_STANDARD 99 C_EXTENSIONS ON)

set(hactool_sources save sha aes extkeys rsa npdm bktr kip packages pki pfs0 hfs0 nca0_romfs romfs utils nax0 nso lz4
                    nca xci filepath ConvertUTF cJSON)
list(TRANSFORM hactool_sources PREPEND "${hactool_src}/")
list(TRANSFORM hactool_sources APPEND .c)
add_library(hactool_lib STATIC ${hactool_sources})
target_include_directories(hactool_lib PUBLIC "${hactool_src}")
target_link_libraries(hactool_lib PUBLIC hactool_mbedtls)
target_compile_definitions(hactool_lib PUBLIC _BSD_SOURCE _POSIX_SOURCE _POSIX_C_SOURCE=200112L _DEFAULT_SOURCE
                                               __USE_MINGW_ANSI_STDIO=1 _FILE_OFFSET_BITS=64)
target_compile_options(hactool_lib PRIVATE -w)
set_target_properties(hactool_lib PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)

add_executable(hactool "${hactool_src}/main.c")
target_link_libraries(hactool PRIVATE hactool_lib)
target_compile_options(hactool PRIVATE -w)
set_target_properties(hactool PROPERTIES C_STANDARD 11 C_EXTENSIONS ON RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
if(WIN32)
    target_link_options(hactool PRIVATE -static)
endif()
