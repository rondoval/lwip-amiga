/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Stack-task internals shared between sb_stack.c (task, netdev bring-up/
 * teardown) and sb_netctl.c (the control-port server that drives them at
 * runtime). Everything here runs on the stack task only.
 */

#ifndef SB_STACK_PRIV_H
#define SB_STACK_PRIV_H

#include <exec/io.h>
#include <exec/ports.h>
#include <exec/types.h>

#include <devices/netdev.h>
#include <netstack_ctl.h>

#include "netdev_if.h"

struct SocketBase;

struct SbStackCtx
{
    struct SocketBase *root;
    struct Task *parent;
    volatile LONG startResult; /* 0 ok, else failed */
    struct NetdevIf ndi;
    struct MsgPort *devPort;
    struct IOStdReq *devIO;
    BOOL devOpen;
    BOOL attached;
    BOOL created; /* netdevif_create succeeded (netif added, glue live) */
    BOOL started;
    BOOL stopRequested; /* sb_stack_stop asked: exit handshake wanted */

    /* control-port bookkeeping (sb_netctl.c): replies deferred past their
     * GetMsg — an ADD_IF waiting for the DHCP lease, a SHUTDOWN waiting for
     * the last client to close */
    struct NetCtlMsg *pendingAdd;
    struct NetCtlMsg *pendingShutdown;

    /* async NIC-stats poll: devIO cycles GET_STATS -> GET_LINK via SendIO so
     * the 100 ms tick never blocks on the driver unit task; results publish
     * into the root cache when the GET_LINK reply lands */
    UBYTE statsPhase; /* 0 idle, 1 GET_STATS out, 2 GET_LINK out */
    struct NetDevStats statsBuf;
    struct NetDevLinkState linkBuf;

    /* off-lock snapshot of ndi_McastList for NETDEV_CMD_SET_RXFILTER: filled
     * under the core lock, then handed to the driver with the lock dropped */
    UBYTE rxFilterMacs[NDIF_MCAST_MAX][6];
};

/* Bring the netdev interface up per @nif: OpenDevice, ATTACH, lwIP netif,
 * START, DHCP or static config, mDNS. Returns a NETCTL_* result; on
 * NETCTL_ERR_DEVICE the device error lands in @aux. Any partial bring-up is
 * unwound before returning, so a failed call leaves no state behind. */
LONG sb_netdev_up(struct SbStackCtx *ctx, const struct NetCtlIfConfig *nif,
                  LONG *aux);

/* Full reverse of sb_netdev_up; safe on any partial state (flag-guarded).
 * The caller must run sb_stats_drain first if the stats cycle may be live. */
void sb_netdev_down(struct SbStackCtx *ctx);

/* Reclaim a stats request still in flight before devIO is reused (STOP/DETACH) */
void sb_stats_drain(struct SbStackCtx *ctx);

#endif /* SB_STACK_PRIV_H */
