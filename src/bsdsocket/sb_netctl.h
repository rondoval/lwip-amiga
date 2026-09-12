/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Netstack control-port server (netstack_ctl.h protocol) — see sb_netctl.c.
 * Everything here runs on the stack task; the module takes the core lock
 * itself around the lwIP raw API.
 */

#ifndef SB_NETCTL_H
#define SB_NETCTL_H

#include <exec/types.h>

struct SbStackCtx;

/* Publish the control port. 0 on success; failure means no runtime
 * interface management, which the caller treats as fatal for startup. */
LONG sb_netctl_start(void);

/* Withdraw the port and answer everything still owed: a parked ADD_IF gets
 * NETCTL_ERR_INACTIVE, then the queue is drained the same way. Safe when
 * never started. (A parked SHUTDOWN is deliberately NOT answered here — its
 * reply is the stack task's very last act; see sb_stack.c.) */
void sb_netctl_stop(struct SbStackCtx *ctx);

/* Control-port signal for the stack task's Wait(), 0 when there is no port. */
ULONG sb_netctl_sigmask(void);

/* Serve every queued control message. */
void sb_netctl_service(struct SbStackCtx *ctx);

/* Per-tick work: complete a parked ADD_IF once its DHCP lease is bound. */
void sb_netctl_tick(struct SbStackCtx *ctx);

/* TRUE when a parked SHUTDOWN can proceed (no clients left). The stack
 * task checks this every loop pass and exits through the teardown path. */
BOOL sb_netctl_shutdown_ready(struct SbStackCtx *ctx);

/* 1 Hz while a shutdown is pending: re-signal the remaining openers'
 * break masks (a mid-Wait client may have eaten the first signal). */
void sb_netctl_nudge(struct SbStackCtx *ctx);

#endif /* SB_NETCTL_H */
