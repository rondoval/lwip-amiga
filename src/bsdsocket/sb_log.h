/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The runtime log facility: where every operational message of the library —
 * client syslog() lines and the stack's own lifecycle/error events — is
 * delivered. Roadshow's public contract (netinclude/libraries/bsdsocket.h):
 * a client installs a struct Hook through SBTC_LOG_HOOK and receives each
 * message as a LogHookMessage, on the context of whoever emitted it.
 *
 * Delivery, in order: the installed hook if any, else the boot-time replay
 * ring and always the debug backend at the debug tiers.
 */

#ifndef SB_LOG_H
#define SB_LOG_H

#include <exec/types.h>
#include <dos/dos.h>       /* struct DateStamp */
#include <utility/hooks.h> /* struct Hook */

#include "netstack_diag.h" /* netstack_log, NS_LOG_* */

struct SocketBase;

/* netinclude/libraries/bsdsocket.h struct LogHookMessage, re-declared here
 * because the library does not include netinclude. Layout asserted in
 * sb_log.c; lhm_Size carries sizeof so a client can detect growth. */
struct SbLogHookMessage
{
    LONG lhm_Size;
    LONG lhm_Priority;         /* LOG_EMERG..LOG_DEBUG, facility stripped */
    struct DateStamp lhm_Date; /* when the line was emitted */
    STRPTR lhm_Tag;            /* origin: the opener's SBTC_LOGTAGPTR, or NULL */
    ULONG lhm_ID;              /* syslog facility of the line (0 for the stack's own) */
    STRPTR lhm_Message;        /* the line, no trailing newline */
};

/* origin of the stack's own lines */
#define SB_LOG_TAG "bsdsocket.library"

/* the replay ring: what a late-starting viewer gets to see of the boot */
#define SB_LOG_RING 16
#define SB_LOG_RING_TAG 32
#define SB_LOG_RING_TEXT 128

/* One client syslog() line, deliberately the same size as a ring entry's text */
#define SB_SYSLOG_BUF SB_LOG_RING_TEXT

struct SbLogEntry
{
    struct DateStamp le_Date;
    ULONG le_Id;
    UBYTE le_Pri;
    char le_Tag[SB_LOG_RING_TAG]; /* "" = no tag */
    char le_Text[SB_LOG_RING_TEXT];
};

/* The stack's own emitter. C format semantics (see netstack_diag.h) */
#define SB_LOG(pri, ...) netstack_log((pri), __VA_ARGS__)

void sb_log_init(struct SocketBase *root); /* LibInit */
void sb_log_exit(struct SocketBase *root); /* LibExpunge */

/* Deliver one line. text is edited in place (a trailing newline is
 * stripped) and must stay valid for the duration of the call only. */
void sb_log_emit(struct SocketBase *root, LONG pri, STRPTR tag, ULONG id, char *text);

/* SBTC_LOG_HOOK set: NULL clears; owner is the opener installing it, so its
 * CloseLibrary can retract a hook it forgot to */
void sb_log_set_hook(struct SocketBase *root, struct Hook *hook, struct SocketBase *owner);
void sb_log_owner_closed(struct SocketBase *root, struct SocketBase *owner);

/* Stack task, once, after netstack_init: subscribes the link/address
 * observer that turns lwIP netif events into log lines. */
void sb_log_netif_attach(void);

#endif /* SB_LOG_H */
