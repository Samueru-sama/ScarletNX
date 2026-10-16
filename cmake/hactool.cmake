# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# hactool (target hactool), used by scarletnx --extract. Its AES, SHA and RSA wrappers run on the build's OpenSSL
# (cmake/hactool-openssl.patch), so the mbedtls 2.x copy hactool bundles is not built.
# hactool as released (hactool_SOURCE_DIR, in CPM's cache) with cmake/hactool-decrypted.patch and
# cmake/hactool-openssl.patch applied, in the build tree; the patched files must have the hashes they had when the
# patches were written (cmake/patched_copy.cmake)
set(hactool_src "${CMAKE_BINARY_DIR}/_deps/hactool-patched")
include("${CMAKE_SOURCE_DIR}/cmake/patched_copy.cmake")
snx_patched_copy(SOURCE "${hactool_SOURCE_DIR}" DEST "${hactool_src}" PATCHES "${hactool_patch}" "${hactool_openssl_patch}"
                 FILES nca.c 7902a46ade61e9801a2a0ad1e98993135e39684ba64dbcbaebccad4d56c7e315
                       xci.c c0ce01f8b433d28979a6f655377b21192d1909996bd625973b1eb28818b56d2e
                       aes.c 0a0fdf15b7232f8c74b4502de2ff97f44e2f2d5f0695d13a5f2304a5d51e0af3
                       aes.h b752de88f873c48c992c6bc25847d53822bde4e1ce8dbf4ecfa987dfda14a958
                       sha.c 7f3d5eccd90107afdd73ee79107788cc832de913d6c13568f18539bd9b159ea2
                       sha.h 712d85c8e42090af85868aa3ba0b5cd48402c51cc6ce2ddc4b57d319999f4ed5
                       rsa.c af6c63b9271dcbba4ef73ff4efd1d0d6691cd00f39170e93b69ddd5e986b7679
                       rsa.h 368a4307276ac6431d6520f45ac6ebdf5b30e6d019a200d4a762003e8216ca25)
# hactool's lz4.c (LZ4 1.8) is left out for the pinned LZ4 (snx_lz4). nso.c still includes its own lz4.h, found first
# beside it, which declares LZ4_decompress_safe, the only LZ4 function hactool calls, as every LZ4 since 1.7 does
set(hactool_sources save sha aes extkeys rsa npdm bktr kip packages pki pfs0 hfs0 nca0_romfs romfs utils nax0 nso nca
                    xci filepath ConvertUTF cJSON)
list(TRANSFORM hactool_sources PREPEND "${hactool_src}/")
list(TRANSFORM hactool_sources APPEND .c)
add_library(hactool_lib STATIC ${hactool_sources})
target_include_directories(hactool_lib PUBLIC "${hactool_src}")
target_link_libraries(hactool_lib PUBLIC OpenSSL::Crypto snx_lz4)
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
# beside the driver in the app's libraries folder, which takes only lib*.so (src/nx/runtime/driver/extract.c)
if(ANDROID)
    set_target_properties(hactool PROPERTIES OUTPUT_NAME libhactool.so)
endif()
