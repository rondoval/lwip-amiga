# SPDX-License-Identifier: BSD-3-Clause
cmake_minimum_required(VERSION 3.16)
project(lwip-amiga C)

# Amiga (m68k) binaries are built through the emu68-driver-stack superproject,
# which provides the toolchain and emu68-common.
if(CMAKE_SYSTEM_NAME STREQUAL "AmigaOS")
    add_subdirectory(src)
endif()
