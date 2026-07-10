# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.16)
project(lwip-amiga C)

# Standalone builds target the Linux host harness only. Amiga (m68k) binaries are
# built through the emu68-driver-stack superproject, which provides the toolchain
# and emu68-common; that wiring arrives with the bsdsocket.library skeleton.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "AmigaOS")
    add_subdirectory(harness)
endif()
