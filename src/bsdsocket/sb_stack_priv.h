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
#include <devices/sana2.h>
#include <netstack_ctl.h>

#include "netdev_if.h"
#include "sana2_if.h"

struct SocketBase;

struct SbStackCtx
{
    struct SocketBase *root;
    struct Task *parent;
    volatile LONG startResult; /* 0 ok, else failed */
    struct NetdevIf ndi;
    struct Sana2If s2i;
    UWORD ifKind; /* NIF_KIND_* of the open interface (set by sb_if_up) */
    struct MsgPort *devPort;
    struct IOStdReq *devIO; /* IOSana2Req-sized superset; netdev commands and
                               the NSD probe use this IOStdReq view, the
                               SANA-II backend casts to struct IOSana2Req */
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

    /* async NIC-stats poll: devIO cycles via SendIO so the stack tick
     * never blocks on the driver; results publish into the root cache
     * (NetDevStats/NetDevLinkState — the neutral shape BOTH backends fill).
     * netdev: GET_STATS -> GET_LINK, two phases; SANA-II: one
     * S2_GETGLOBALSTATS phase into s2StatsBuf, mapped + merged with the
     * glue's byte counters at reply. */
    UBYTE statsPhase; /* 0 idle, 1 first request out, 2 GET_LINK out (netdev) */
    struct NetDevStats statsBuf;
    struct NetDevLinkState linkBuf;
    struct Sana2DeviceStats s2StatsBuf;

    /* RX profile (NETDEV_CMD_SET_RX_PROFILE, drivers with NDCF_RX_PROFILE).
     * The decision is root->rxProfileStated; these are its delivery — a
     * request of its own, cloned from devIO, because the profile must reach
     * the driver while devIO is out on a stats cycle, and asynchronous like
     * everything else the stack task sends the driver. */
    struct IOStdReq rxProfileIO;
    struct NetDevRxProfile rxProfileBuf;
    UBYTE rxProfileInFlight; /* rxProfileIO is out; its reply is still to come */
    UBYTE rxProfileSent;     /* NDRP_* the driver has, or is being sent; UNSTATED = nothing yet */

    /* the tick's sample of the receive side, the other half of the decision */
    UBYTE rxWasBusy;    /* the last tick's frame delta reached SB_RX_BUSY_FRAMES */
    ULONG rxFramesPrev; /* ndi_RxFrames as of that tick */

    /* The signal this task allocated at startup for openers to raise when one
     * of them blocks awaiting a reply; 0 = none was free, and the profile is
     * then decided on the tick alone. root->rxAwaitSig is the same mask
     * published where openers can see it, and only while a driver is listening
     * — cleared at every teardown, which is why the allocation is kept here
     * too: it is what the next bring-up republishes from. */
    ULONG rxAwaitSigOwned;

    /* off-lock snapshot of the base's joined-MAC set: filled under the core
     * lock (netifbase_mcast_snapshot), then handed to the driver with the
     * lock dropped — netdev as one declarative NETDEV_CMD_SET_RXFILTER,
     * SANA-II as S2_ADD/DELMULTICASTADDRESS deltas against sanaShadow (the
     * last set actually programmed). */
    UBYTE rxFilterMacs[NIB_MCAST_MAX][6];
    UBYTE sanaShadow[NIB_MCAST_MAX][6];
    UWORD sanaShadowCount;
    BOOL sanaMcastUnsupported; /* driver said IOERR_NOCMD: stop trying */
};

/* The interface base of whichever backend owns (or is bringing up) the
 * interface — what backend-agnostic code dereferences instead of ctx->ndi. */
static inline struct NetIfBase *sb_ctx_base(struct SbStackCtx *ctx)
{
    return ctx->ifKind == NIF_KIND_SANA2 ? &ctx->s2i.s2i_Base
                                         : &ctx->ndi.ndi_Base;
}

/* Bring an interface up per @nif: open the device, resolve the driver ABI
 * (explicit TYPE, or the NSCMD_DEVICEQUERY probe for AUTO) and hand off to
 * the backend bring-up. Returns a NETCTL_* result; on NETCTL_ERR_DEVICE the
 * device error lands in @aux. Any partial bring-up is unwound before
 * returning, so a failed call leaves no state behind. */
LONG sb_if_up(struct SbStackCtx *ctx, const struct NetCtlIfConfig *nif,
              LONG *aux);

/* Full reverse of sb_if_up (backend teardown by ctx->ifKind, then the
 * common device close); safe on any partial state (flag-guarded). The
 * caller must run sb_stats_drain first if the stats cycle may be live. */
void sb_if_down(struct SbStackCtx *ctx);

/* Shared bring-up steps for the backends (sb_stack.c): identify = identity
 * stamp (call right after the backend's create added the netif, before
 * anything can raise netif events); configure = netif default/hostname/
 * address/up (call once the datapath can carry the frames set_up emits);
 * services = DHCP + mDNS (call once the driver is started). */
void sb_if_identify(struct SbStackCtx *ctx, const struct NetCtlIfConfig *nif);
void sb_if_configure(struct SbStackCtx *ctx, const struct NetCtlIfConfig *nif);
void sb_if_services(struct SbStackCtx *ctx, const struct NetCtlIfConfig *nif);

/* Backend halves, dispatched by sb_if_up/sb_if_down. Contract: the device
 * is already open on ctx->devIO when *_up runs, and a failed *_up unwinds
 * everything (through sb_if_down) before returning; *_down handles only the
 * backend's own stages — the device close belongs to sb_if_down. */
LONG sb_netdev_up(struct SbStackCtx *ctx, const struct NetCtlIfConfig *nif,
                  LONG *aux);
void sb_netdev_down(struct SbStackCtx *ctx);
LONG sb_sana_up(struct SbStackCtx *ctx, const struct NetCtlIfConfig *nif,
                LONG *aux);
void sb_sana_down(struct SbStackCtx *ctx);

/* Tick-driven backend services (sb_netdev.c / sb_sana.c), called through
 * the kind dispatchers in the stack task's loop. */
void sb_netdev_stats_kick(struct SbStackCtx *ctx);
void sb_netdev_stats_reply(struct SbStackCtx *ctx);
void sb_netdev_rxfilter_sync(struct SbStackCtx *ctx);
/* RX profile: _tick samples the receive side and decides, _decide only decides
 * (the "somebody awaits a reply" signal), both push a change; _reply reclaims
 * the request; _drain before STOP/DETACH, like the stats request. */
void sb_netdev_profile_tick(struct SbStackCtx *ctx);
void sb_netdev_profile_decide(struct SbStackCtx *ctx);
void sb_netdev_profile_reply(struct SbStackCtx *ctx);
void sb_netdev_profile_drain(struct SbStackCtx *ctx);
void sb_sana_stats_kick(struct SbStackCtx *ctx);
void sb_sana_stats_reply(struct SbStackCtx *ctx);
void sb_sana_mcast_sync(struct SbStackCtx *ctx);

/* Reclaim a stats request still in flight before devIO is reused (STOP/DETACH) */
void sb_stats_drain(struct SbStackCtx *ctx);

#endif /* SB_STACK_PRIV_H */
