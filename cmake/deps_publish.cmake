# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
#
#   cmake -DFROM=<installed prefix> -DTO=<SNX_DEPS_CACHE>/<name>-<hash> -DKEY=<key file> -P cmake/deps_publish.cmake
#
# Puts a finished FFmpeg or OpenSSL install in the dependency cache (cmake/deps_cache.cmake) whole or not at all: it is
# copied into a folder of its own, marked complete, and renamed into place last, so no tree finds half of one. When
# another tree published the same build meanwhile, this copy is dropped.
foreach(var FROM TO KEY)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "deps_publish.cmake needs -D${var}=...")
    endif()
endforeach()
if(EXISTS "${TO}/snx-complete")
    return()
endif()
string(RANDOM LENGTH 8 tag)
set(partial "${TO}.partial-${tag}")
file(COPY "${FROM}/" DESTINATION "${partial}")
file(COPY_FILE "${KEY}" "${partial}/snx-complete")
# A folder is not renamed over another that holds files (NO_REPLACE works on files only): a published one stays
file(RENAME "${partial}" "${TO}" RESULT failed)
if(failed)
    file(REMOVE_RECURSE "${partial}")
endif()
