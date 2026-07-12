# SPDX-License-Identifier: GPL-2.0-or-later
set(LWIP_DIR ${CMAKE_SOURCE_DIR}/lwip)

if(NOT EXISTS ${LWIP_DIR}/src/Filelists.cmake)
    message(FATAL_ERROR "lwIP submodule missing — run: git submodule update --init")
endif()

include(${LWIP_DIR}/src/Filelists.cmake)

add_executable(smoke smoke.c ${lwipcore_SRCS} ${lwipcore4_SRCS}
    ${LWIP_DIR}/src/netif/ethernet.c)
target_include_directories(smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${LWIP_DIR}/src/include
)
target_compile_options(smoke PRIVATE -Wall -Wextra)

# tcpbench compiles the core again with its own lwipopts (window scaling,
# SACK, no loopif) — bench/ must precede this dir on the include path.
add_executable(tcpbench bench/tcpbench.c bench/echoif.c
    ${lwipcore_SRCS} ${lwipcore4_SRCS}
    ${LWIP_DIR}/src/netif/ethernet.c)
target_include_directories(tcpbench PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/bench
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${LWIP_DIR}/src/include
)
target_compile_options(tcpbench PRIVATE -Wall -Wextra)
