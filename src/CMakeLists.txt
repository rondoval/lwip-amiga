# SPDX-License-Identifier: BSD-3-Clause
# bsdsocket.library — Amiga-side build (via the emu68-driver-stack superproject).
project(bsdsocket.library VERSION 4.0 LANGUAGES C)

# bsdsocket.library's runtime version. The ABI major stays 4 (the AmiTCP/Roadshow
# contract apps test via OpenLibrary("bsdsocket.library", 4)); the revision is derived
# from the component release version (top-level project(lwip-amiga VERSION x.y)) so it
# increments monotonically with each release and survives a major bump:
# 0.1 -> 4.1, 0.2 -> 4.2, 1.0 -> 4.100, 5.3 -> 4.503. Assumes minor < 100 (lib_Revision
# is a UWORD, so major up to ~655 fits).
set(BSDSOCKET_ABI_MAJOR ${PROJECT_VERSION_MAJOR})
if(NOT DEFINED lwip-amiga_VERSION_MAJOR)
    set(lwip-amiga_VERSION_MAJOR 0)
    set(lwip-amiga_VERSION_MINOR 0)
endif()
math(EXPR BSDSOCKET_REVISION "${lwip-amiga_VERSION_MAJOR} * 100 + ${lwip-amiga_VERSION_MINOR}")

# Version string (fleet convention)
string(TIMESTAMP CURRENT_DATE "(%d.%m.%Y)")
set(VERSTRING "$VER: ${PROJECT_NAME} ${BSDSOCKET_ABI_MAJOR}.${BSDSOCKET_REVISION} ${CURRENT_DATE}")

# Shared warning flags. -m68040 / -fomit-frame-pointer are set stack-wide by the
# toolchain (cmake/toolchain.cmake) and are not repeated per target.
set(LWIP_WARN -Wall -Wextra)

find_package(Emu68Common CONFIG REQUIRED)

# Backend compile definitions (DEBUG/DEBUG_SERIAL/DEBUG_HIGH) apply only to
# targets created after this call — it must precede every target in this dir.
emu68_debug_backend_definitions()

# --- netstack: lwIP core + Amiga port layer ---------------------------------
# The stack proper as a static library: unmodified lwIP compiled with the
# real port configuration (port/amiga/include/lwipopts.h) plus the port
# sources (core lock/time/heap, netdev netif glue).
set(LWIP_DIR ${CMAKE_SOURCE_DIR}/lwip)
include(${LWIP_DIR}/src/Filelists.cmake)

add_library(netstack STATIC
    ${lwipcore_SRCS}
    ${lwipcore4_SRCS}
    ${LWIP_DIR}/src/netif/ethernet.c
    ${CMAKE_SOURCE_DIR}/port/amiga/netstack.c
    ${CMAKE_SOURCE_DIR}/port/amiga/netdev_if.c
)

target_compile_options(netstack PRIVATE -O3 ${LWIP_WARN})

target_include_directories(netstack
    PUBLIC
        ${CMAKE_SOURCE_DIR}/port/amiga/include
        ${CMAKE_SOURCE_DIR}/include
        ${LWIP_DIR}/src/include
)

target_link_libraries(netstack
    PRIVATE
        Emu68Common::common
)

# --- bsdsocket.library ------------------------------------------------------
# Source order matters: main.c first (doNotExecute entry), end.c last
# (endOfCode marker bounds the resident structure).
add_executable(bsdsocket_library
    bsdsocket/main.c
    bsdsocket/vectors.c
    bsdsocket/sb_stack.c
    bsdsocket/sb_socket.c
    bsdsocket/sb_api.c
    bsdsocket/sb_select.c
    bsdsocket/sb_misc.c
    bsdsocket/sb_misc2.c
    bsdsocket/sb_gai.c
    bsdsocket/end.c
)

target_compile_definitions(bsdsocket_library
    PRIVATE
        LIBRARY_IDSTRING="${VERSTRING}"
        LIBRARY_VERSION=${BSDSOCKET_ABI_MAJOR}
        LIBRARY_REVISION=${BSDSOCKET_REVISION}
        LIBRARY_NAME="${PROJECT_NAME}"
)

target_compile_options(bsdsocket_library
    PRIVATE
        -O3
        ${LWIP_WARN}
        -Wshadow
        -Wmissing-prototypes
        -Wstrict-prototypes
)

# libnix supplies the libc bits (ctype, string, atoi) — but no startup code:
# a library binary must not carry crt0, so the entry stays doNotExecute and
# nothing may depend on constructor-initialized libc state (the lwIP heap is
# routed to netstack_malloc, never to libc malloc).
target_link_options(bsdsocket_library
    PRIVATE
        -nostartfiles
        -s
        -Wl,-e,_doNotExecute
)

target_link_libraries(bsdsocket_library
    PRIVATE
        netstack
        Emu68Common::common
)

emu68_debug_backend_finalize(bsdsocket_library)

set_target_properties(bsdsocket_library PROPERTIES
    OUTPUT_NAME bsdsocket
    SUFFIX ".library"
)

# The netdev ABI header is installed and exported as the Netdev cmake package —
# that is how genet.device consumes the contract.
install(FILES ${CMAKE_SOURCE_DIR}/include/netdev.h DESTINATION include)

add_library(netdev_headers INTERFACE)
target_include_directories(netdev_headers
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

install(
    TARGETS netdev_headers
    EXPORT NetdevTargets
)

install(
    EXPORT NetdevTargets
    NAMESPACE Netdev::
    FILE NetdevTargets.cmake
    DESTINATION lib/cmake/Netdev
)

include(CMakePackageConfigHelpers)

configure_package_config_file(
    ${CMAKE_SOURCE_DIR}/cmake/NetdevConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/NetdevConfig.cmake
    INSTALL_DESTINATION lib/cmake/Netdev
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/NetdevConfigVersion.cmake
    VERSION 1.0
    COMPATIBILITY SameMajorVersion
)

install(
    FILES
        ${CMAKE_CURRENT_BINARY_DIR}/NetdevConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/NetdevConfigVersion.cmake
    DESTINATION lib/cmake/Netdev
)

# --- netdev-stats: loss-point counters on release builds --------------------
# Second opener beside the running stack (GET_STATS/GET_LINK/SET_COALESCE
# need no ATTACH) — the perf-tuning eyes when the DEBUG mib/txh prints are
# compiled out. Pure exec/dos + the ABI header; no netstack, no debug glue.
add_executable(netdev-stats
    netdev-stats/netdev-stats.c
)

target_compile_options(netdev-stats PRIVATE -O2 ${LWIP_WARN})

target_link_libraries(netdev-stats
    PRIVATE
        netdev_headers
)

install(
    PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/netdev-stats
    DESTINATION C
)

# --- socktest: bsdsocket.library end-to-end test ----------------------------
# App-side exercise of the socket layer: DNS + TCP connect/send/recv through
# the library's LVOs (own inline glue; no netinclude dependency).
add_executable(socktest
    socktest/socktest.c
)

target_compile_options(socktest PRIVATE -O2 ${LWIP_WARN})

target_link_libraries(socktest
    PRIVATE
        Emu68Common::common
)

emu68_debug_backend_finalize(socktest)

# socktest is a developer exerciser — built for local HW testing but deliberately
# not installed, so it stays out of the shipped package.

# The shippable library.
install(
    PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/bsdsocket.library
    DESTINATION LIBS
)
