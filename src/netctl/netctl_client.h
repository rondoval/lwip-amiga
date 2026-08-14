/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Shared client side of the netstack control port (netstack_ctl.h) for the
 * AddNetInterface / RemoveNetInterface / NetShutdown commands.
 */

#ifndef NETCTL_CLIENT_H
#define NETCTL_CLIENT_H

#include <exec/ports.h>
#include <exec/types.h>

#include <netstack_ctl.h>

/* Zero @msg and fill the message plumbing: type, length, reply port,
 * protocol version, @op. ncm_Result is preset to NETCTL_ERR_INACTIVE so an
 * undelivered message reads as such. */
void netctl_msg_init(struct NetCtlMsg *msg, struct MsgPort *reply, UWORD op);

/* Deliver @msg to the stack's control port. The Forbid() spans find+send so
 * the port cannot be withdrawn in between. FALSE = no port (stack not
 * running); the message was NOT sent. */
BOOL netctl_send(struct NetCtlMsg *msg);

/* Collect exactly @count replies from @reply. The port is private to the
 * caller, so every message that arrives is one of its own coming back; the
 * server guarantees every delivered message a reply (drain-on-teardown), so
 * this always terminates. Used after a cancel, when two messages (the
 * parked op + the cancel) are in flight in either order and both must be
 * back before their stack frames may be reused. */
void netctl_drain(struct MsgPort *reply, ULONG count);

/* Dotted quad -> network-byte-order ULONG (strict: exactly 4 octets). */
BOOL netctl_aton(const char *s, ULONG *out);

/* Network-byte-order ULONG -> dotted quad; @buf holds >= 16 bytes. */
void netctl_ntoa(ULONG addr, char *buf);

/* Human-readable NETCTL_* result (generic; commands special-case the
 * context-dependent ones like ERR_EXISTS before falling back to this). */
const char *netctl_strerror(LONG result);

#endif /* NETCTL_CLIENT_H */
