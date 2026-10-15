# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
#
# OpenSSL's libcrypto and libssl as static libraries (targets OpenSSL::Crypto and OpenSSL::SSL), built from the release
# source in cmake/dependencies.cmake with OpenSSL's own Configure (Perl) and make. libcrypto does the hashing and
# ciphers (NSO, NRR and 3DS hashes, csrng, hactool's AES, SHA and RSA), libssl the ssl service's TLS. Only the libraries
# are built: no programs, docs, tests or loadable modules, and no configuration file is read at startup, so the result
# needs nothing but libc and behaves the same everywhere. OpenSSL is Apache 2.0. It needs Perl and make (gmake on the
# BSDs).
# This file is included into the top-level scope: its variables carry the snx_openssl_ prefix.
set(snx_openssl_src "${OpenSSL_SOURCE_DIR}")
set(snx_openssl_prefix "${CMAKE_BINARY_DIR}/openssl")
find_package(Perl)
find_program(SNX_OPENSSL_MAKE NAMES gmake make)
if(NOT PERL_FOUND OR NOT SNX_OPENSSL_MAKE)
    message(FATAL_ERROR "Building OpenSSL needs Perl and GNU make (gmake on the BSDs) on the PATH. On Windows, Perl "
                        "comes with Git for Windows or Strawberry Perl. OpenSSL is required.")
endif()

# Configure guesses the platform from the machine doing the build, which is right for a native build. A Windows build
# (native with llvm-mingw, or cross) names it: mingw64 is OpenSSL's target for 64-bit MinGW, x86-64 or ARM64. Android's
# targets find the NDK's Clang on PATH and name the compiler themselves (<triple><API>-clang), from ANDROID_NDK_ROOT
# and the API level, so that build passes those instead of CC, AR and RANLIB
set(snx_openssl_target "")
set(snx_openssl_env "")
if(ANDROID)
    if(ANDROID_ABI STREQUAL "arm64-v8a")
        set(snx_openssl_target android-arm64)
    elseif(ANDROID_ABI STREQUAL "x86_64")
        set(snx_openssl_target android-x86_64)
    else()
        message(FATAL_ERROR "OpenSSL's Android build is only set up for arm64-v8a and x86_64 (cmake/openssl.cmake)")
    endif()
    set(snx_openssl_env "${CMAKE_COMMAND}" -E env "ANDROID_NDK_ROOT=${CMAKE_ANDROID_NDK}"
                        "PATH=${ANDROID_TOOLCHAIN_ROOT}/bin:$ENV{PATH}")
elseif(WIN32)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
        set(snx_openssl_target mingw-arm64)
    else()
        set(snx_openssl_target mingw64)
    endif()
elseif(CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR "Cross-building OpenSSL is only set up for Windows and Android (cmake/openssl.cmake)")
endif()
set(snx_openssl_args
    ${snx_openssl_target}
    --prefix=${snx_openssl_prefix}
    --libdir=lib
    no-shared
    no-module
    no-apps
    no-docs
    no-tests
    no-autoload-config)
if(ANDROID)
    list(APPEND snx_openssl_args -D__ANDROID_API__=${ANDROID_PLATFORM_LEVEL})
else()
    list(APPEND snx_openssl_args CC=${CMAKE_C_COMPILER} AR=${CMAKE_AR} RANLIB=${CMAKE_RANLIB})
endif()
if(CMAKE_RC_COMPILER)
    list(APPEND snx_openssl_args RC=${CMAKE_RC_COMPILER})
endif()
if(NOT WIN32)
    # position-independent, as FFmpeg is: the libraries go into a shared library too (Android's runtime)
    list(APPEND snx_openssl_args -fPIC)
endif()

# OpenSSL's make does not see the build's -j
cmake_host_system_information(RESULT snx_openssl_jobs QUERY NUMBER_OF_LOGICAL_CORES)
if("$ENV{CMAKE_BUILD_PARALLEL_LEVEL}" MATCHES "^[1-9][0-9]*$")
    set(snx_openssl_jobs "$ENV{CMAKE_BUILD_PARALLEL_LEVEL}")
endif()
include(ExternalProject)
ExternalProject_Add(openssl_build
    SOURCE_DIR "${snx_openssl_src}"
    BINARY_DIR "${CMAKE_BINARY_DIR}/openssl-build"
    INSTALL_DIR "${snx_openssl_prefix}"
    CONFIGURE_COMMAND ${snx_openssl_env} "${PERL_EXECUTABLE}" "${snx_openssl_src}/Configure" ${snx_openssl_args}
    BUILD_COMMAND ${snx_openssl_env} "${SNX_OPENSSL_MAKE}" -j${snx_openssl_jobs} build_libs
    # install_dev: the headers and libraries, without the programs and manuals
    INSTALL_COMMAND ${snx_openssl_env} "${SNX_OPENSSL_MAKE}" install_dev
    BUILD_BYPRODUCTS "${snx_openssl_prefix}/lib/libcrypto.a" "${snx_openssl_prefix}/lib/libssl.a"
    LOG_CONFIGURE ON
    LOG_BUILD ON
    LOG_INSTALL ON
    LOG_OUTPUT_ON_FAILURE ON)

# The include folder is filled by the install step, but has to exist for the imported targets at configure time
file(MAKE_DIRECTORY "${snx_openssl_prefix}/include")
add_library(OpenSSL::Crypto STATIC IMPORTED GLOBAL)
# SYSTEM OFF, as for FFmpeg: an -I that holds the system's OpenSSL headers (/usr/local/include on the BSDs) would win
# over -isystem ones
set_target_properties(OpenSSL::Crypto PROPERTIES IMPORTED_LOCATION "${snx_openssl_prefix}/lib/libcrypto.a"
                                                 INTERFACE_INCLUDE_DIRECTORIES "${snx_openssl_prefix}/include"
                                                 SYSTEM OFF)
add_dependencies(OpenSSL::Crypto openssl_build)
if(WIN32)
    # the random seed (BCryptGenRandom), the Windows certificate store, and sockets for BIO
    target_link_libraries(OpenSSL::Crypto INTERFACE bcrypt crypt32 ws2_32 advapi32 user32)
else()
    target_link_libraries(OpenSSL::Crypto INTERFACE Threads::Threads ${CMAKE_DL_LIBS})
endif()
add_library(OpenSSL::SSL STATIC IMPORTED GLOBAL)
set_target_properties(OpenSSL::SSL PROPERTIES IMPORTED_LOCATION "${snx_openssl_prefix}/lib/libssl.a" SYSTEM OFF)
target_link_libraries(OpenSSL::SSL INTERFACE OpenSSL::Crypto)
add_dependencies(OpenSSL::SSL openssl_build)
