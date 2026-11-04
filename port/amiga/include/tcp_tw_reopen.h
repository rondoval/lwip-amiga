/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Reopen a TCP connection from TIME-WAIT on a new SYN (RFC 1122 4.2.2.13).
 *
 * lwIP answers any SYN matching a TIME-WAIT pcb with an ACK (or a RST when the
 * SYN is in window) and ignores the peer's RST (RFC 1337), so a client reusing
 * its source port within 2*TCP_MSL (120 s) cannot connect: its connect() stalls
 * until the pcb expires, or times out. Linux changes its ephemeral port area
 * about every 10 s, so this happens at any connection rate.
 *
 * BSD and Linux accept such a SYN when its sequence number is above the end of
 * the previous incarnation. lwIP has no knob for it and is not patched here:
 * this netif input function removes the matching TIME-WAIT pcb before the frame
 * reaches ethernet_input(), and tcp_input() then finds the listener as for any
 * new connection.
 *
 * It is an input function and not lwIP's IP4 input hook because that hook, being
 * inside ip4_input() under LTO, moved the whole image and cost 3 % on small TCP
 * request/response (measured, V4). This file is built without LTO for the same
 * reason. Frames with a VLAN tag keep lwIP's TIME-WAIT behaviour.
 *
 * Everything read here comes before any header check, so it is bounds-checked.
 * The TCP checksum is not verified: a damaged or forged SYN that matches the
 * tuple only ends that TIME-WAIT early, and is then dropped or answered by
 * tcp_input().
 */
#ifndef TCP_TW_REOPEN_H
#define TCP_TW_REOPEN_H

#include "lwip/err.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"

err_t tcp_tw_reopen_input(struct pbuf *p, struct netif *inp);

#endif /* TCP_TW_REOPEN_H */
