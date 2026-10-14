# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# The AGB (Game Boy Advance) board runtime: the board emulation scarletnx-agb runs GBA ROMs on. The board library is
# CPU-only and independent of the NX and GNU ABI runtimes; the executable is its entry point.
#
#   scarletnx-agb-runtime   the board: cartridge, memory map, devices
#   scarletnx-agb-run       the system type's executable, named scarletnx-agb

add_library(scarletnx-agb STATIC board/board.c cartridge/cartridge.c)
set_target_properties(scarletnx-agb PROPERTIES OUTPUT_NAME scarletnx-agb-runtime C_STANDARD 23 C_STANDARD_REQUIRED ON)
target_include_directories(scarletnx-agb PUBLIC "${PROJECT_SOURCE_DIR}/src/agb" "${PROJECT_SOURCE_DIR}/src/common")
target_link_libraries(scarletnx-agb PUBLIC scarletnx-common)
target_compile_options(scarletnx-agb PRIVATE -Wall -Wextra -Werror)

add_executable(scarletnx-agb-run runtime/main.c)
target_link_libraries(scarletnx-agb-run PRIVATE scarletnx-agb)
set_target_properties(scarletnx-agb-run PROPERTIES OUTPUT_NAME scarletnx-agb
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}" C_STANDARD 23 C_STANDARD_REQUIRED ON)

if(BUILD_TESTING)
    add_executable(snx-test-agb-board "${PROJECT_SOURCE_DIR}/tests/agb_board.c")
    target_link_libraries(snx-test-agb-board PRIVATE scarletnx-agb)
    target_compile_options(snx-test-agb-board PRIVATE -Wall -Wextra -Werror)
    set_target_properties(snx-test-agb-board PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON)
    add_test(NAME agb_board COMMAND snx-test-agb-board)
endif()

include(GNUInstallDirs)
install(TARGETS scarletnx-agb-run RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
