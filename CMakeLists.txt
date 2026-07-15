# SPDX-License-Identifier: BSD-3-Clause
cmake_minimum_required(VERSION 3.14)
# The component (release) version — the single knob the versioning workflow tags
# and the stack harvests. The bsdsocket.library ABI version is separate: it keeps
# major 4 (the AmiTCP/Roadshow contract) with its revision derived from this
# version in src/CMakeLists.txt.
project(lwip-amiga VERSION 0.1 LANGUAGES C)

# Amiga (m68k) binaries are built through the emu68-driver-stack superproject,
# which provides the toolchain and emu68-common.
add_subdirectory(src)
