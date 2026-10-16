# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
#
#   cmake -DSRC=<source dir> -DWORK=<scratch dir> -P tests/patched_copy_test.cmake
#
# A package's patched copy (cmake/patched_copy.cmake): made from the cache's copy as released, which stays as it is;
# kept while every patched file has its hash, made again when one differs; a patch that does not apply, or that applies
# and makes something else, stops the configure.
foreach(var SRC WORK)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "patched_copy_test.cmake needs -D${var}=...")
    endif()
endforeach()
include("${SRC}/cmake/patched_copy.cmake")
function(fail what)
    message(FATAL_ERROR "patched_copy: ${what}")
endfunction()
# a fixture's bytes, the same on every system: file(WRITE) ends its lines otherwise on Windows than patch(1) does there
function(put path text)
    file(CONFIGURE OUTPUT "${path}" CONTENT "${text}" NEWLINE_STYLE UNIX)
endfunction()

# the cases that must stop, each in a run of its own
if(DEFINED CASE)
    file(READ "${WORK}/two.sha256" two)
    snx_patched_copy(SOURCE "${WORK}/released" DEST "${WORK}/${CASE}" PATCHES "${WORK}/${CASE}.patch" FILES a.txt ${two})
    return()
endif()

file(REMOVE_RECURSE "${WORK}")
set(lines "1\n2\n3\n4\n5\n6\n7\n")
put("${WORK}/released/a.txt" "${lines}one\n")
put("${WORK}/released/sub/b.txt" "kept\n")
put("${WORK}/expected/a.txt" "${lines}two\n")
file(SHA256 "${WORK}/expected/a.txt" two)
file(WRITE "${WORK}/two.sha256" "${two}")
put("${WORK}/fix.patch" "--- a/a.txt\n+++ b/a.txt\n@@ -6,3 +6,3 @@\n 6\n 7\n-one\n+two\n")
put("${WORK}/stale.patch" "--- a/a.txt\n+++ b/a.txt\n@@ -6,3 +6,3 @@\n 6\n 7\n-zero\n+two\n")
put("${WORK}/other.patch" "--- a/a.txt\n+++ b/a.txt\n@@ -6,3 +6,3 @@\n 6\n 7\n-one\n+uno\n")
set(copy "${WORK}/patched")
macro(patch_it)
    snx_patched_copy(SOURCE "${WORK}/released" DEST "${copy}" PATCHES "${WORK}/fix.patch" FILES a.txt ${two})
endmacro()

# made, the whole tree copied, the released copy left as it was
patch_it()
file(READ "${copy}/a.txt" got)
file(READ "${WORK}/released/a.txt" released)
if(NOT got STREQUAL "${lines}two\n" OR NOT EXISTS "${copy}/sub/b.txt" OR NOT released STREQUAL "${lines}one\n")
    fail("the copy is not the released tree patched, or the released copy changed")
endif()
# kept while it holds: a file only the copy has stays
file(WRITE "${copy}/marker" "")
patch_it()
if(NOT EXISTS "${copy}/marker")
    fail("a copy whose patched files have their hashes was made again")
endif()
# a patched file that differs (a patch edited since, a copy left half patched): made again from the released one
put("${copy}/a.txt" "${lines}tw\n")
patch_it()
file(READ "${copy}/a.txt" got)
if(EXISTS "${copy}/marker" OR NOT got STREQUAL "${lines}two\n")
    fail("a copy with a file that differs was not made again")
endif()
# a patch that does not apply, and one that applies and makes something else, stop the configure saying so
foreach(case_said "stale;does not apply" "other;is not what the patches make")
    list(GET case_said 0 case)
    list(GET case_said 1 said)
    execute_process(COMMAND "${CMAKE_COMMAND}" "-DSRC=${SRC}" "-DWORK=${WORK}" "-DCASE=${case}" -P "${CMAKE_CURRENT_LIST_FILE}"
                    RESULT_VARIABLE failed OUTPUT_VARIABLE out ERROR_VARIABLE out)
    string(FIND "${out}" "${said}" at)
    if(NOT failed OR at LESS 0)
        fail("${case}.patch: the configure went on, or said otherwise:\n${out}")
    endif()
endforeach()
message(STATUS "patched_copy passed")
