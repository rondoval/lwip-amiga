#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Build the lwIP TCP-sender fuzzer (host gcc + ASAN) against ../../lwip.
# Compiles lwIP core + lwIP's own unit-test helper (tcp_helper.c, BSD-3)
# straight from the submodule; nothing is vendored here.
set -e
HERE=$(dirname "$(readlink -f "$0")")
LWIP=$(readlink -f "$HERE/../../lwip")

SRCS="
$LWIP/src/core/init.c
$LWIP/src/core/def.c
$LWIP/src/core/mem.c
$LWIP/src/core/memp.c
$LWIP/src/core/netif.c
$LWIP/src/core/pbuf.c
$LWIP/src/core/stats.c
$LWIP/src/core/inet_chksum.c
$LWIP/src/core/ip.c
$LWIP/src/core/tcp.c
$LWIP/src/core/tcp_in.c
$LWIP/src/core/tcp_out.c
$LWIP/src/core/ipv4/ip4.c
$LWIP/src/core/ipv4/ip4_addr.c
$LWIP/test/unit/tcp/tcp_helper.c
$HERE/fuzz_oversize.c
"

gcc -O1 -g -fsanitize=address,undefined -fno-sanitize=alignment -fno-omit-frame-pointer \
    -Wall -Wextra -Wno-unused-parameter \
    -I"$HERE" -I"$HERE/shim" -I"$LWIP/src/include" -I"$LWIP/test/unit/tcp" \
    $SRCS -o "$HERE/fuzz_oversize"
echo "built: $HERE/fuzz_oversize"
