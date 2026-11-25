#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# Build the TIME-WAIT reopen tests (host gcc + ASAN) against ../../lwip:
#   twreopen_test      with LWIP_TCP_TW_REOPEN, the lwIP fork's option
#   twreopen_test_off  without it: the reopen checks must fail (run it with "off")
set -e
HERE=$(dirname "$(readlink -f "$0")")
ROOT=$(readlink -f "$HERE/../..")
LWIP=$ROOT/lwip
FUZZ=$ROOT/test/fuzz

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
$HERE/twreopen_test.c
"

FLAGS="-O1 -g -fsanitize=address,undefined -fno-sanitize=alignment -fno-omit-frame-pointer \
    -Wall -Wextra -Wno-unused-parameter \
    -I$HERE -I$FUZZ/shim -I$LWIP/src/include"

gcc $FLAGS $SRCS -o "$HERE/twreopen_test"
gcc $FLAGS -DLWIP_TCP_TW_REOPEN=0 $SRCS -o "$HERE/twreopen_test_off"
echo "built: $HERE/twreopen_test $HERE/twreopen_test_off"
