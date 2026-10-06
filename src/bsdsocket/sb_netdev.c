/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * netdev backend, exec side: bring-up/teardown of an interface whose driver
 * speaks the netdev ABI (devices/netdev.h), dispatched from sb_if_up/down,
 * plus the netdev flavors of the tick-driven services — the async NIC-stats
 * poll and the declarative multicast RX-filter push. The datapath glue
 * lives in port/amiga/netdev_*.c.
 */

#include "sb_base.h"

#include <exec/io.h>

#include <debug.h>
#include <memory.h>

#include <lwip/dhcp.h>
#include <lwip/netif.h>

#include <devices/netdev.h>
#include "netdev_if.h"
#include "netstack.h"
#include "sb_mdns.h"
#include "sb_stack_priv.h"

static BYTE sb_netdev_cmd(struct IOStdReq *io, UWORD cmd, APTR data, ULONG len)
{
    KprintfT("[bsdsocket] %s: cmd 0x%04lx, len %lu\n", __func__, (ULONG)cmd, len);
    io->io_Command = cmd;
    io->io_Data = data;
    io->io_Length = len;
    io->io_Actual = 0;
    DoIO((struct IORequest *)io);
    return io->io_Error;
}

/* The asynchronous twin, for everything issued from the tick loop: that loop
 * must never block on the driver unit task (a DoIO here would stall lwIP timer
 * servicing for the whole round trip — worst exactly when the link is busy),
 * and a netdev command must never be issued under netstack_lock, which the unit
 * task takes in its RX path. Both callers below harvest the reply on a later
 * tick instead. */
static void sb_netdev_send(struct IOStdReq *io, UWORD cmd, APTR data, ULONG len)
{
    KprintfT("[bsdsocket] %s: cmd 0x%04lx, len %lu\n", __func__, (ULONG)cmd, len);
    io->io_Command = cmd;
    io->io_Data = data;
    io->io_Length = len;
    io->io_Actual = 0;
    SendIO((struct IORequest *)io);
}

/*
 * RX profile: the stack's half of NETDEV_CMD_SET_RX_PROFILE.
 *
 * The driver can batch received frames for throughput or deliver a lone frame
 * at once, and cannot know which is wanted; this layer can. The policy:
 *
 *   LATENCY is the normal state. It costs an idle or lightly loaded receive
 *   side nothing - sparse frames take one interrupt each either way, only
 *   earlier - and it is what every request/response conversation wants: SMB,
 *   NFS, DNS, ping, an interactive session, a server on this machine.
 *
 *   THROUGHPUT is stated when the receive side is busy (SB_RX_BUSY_FRAMES or
 *   more per tick) and no task has blocked waiting for a reply for
 *   SB_RX_AWAIT_TICKS: a stream in one direction - a download nobody polls, or
 *   the ACKs of an upload, which under the latency profile cost an interrupt
 *   per handful of ACKs and a fifth of the send rate.
 *
 * "Waiting for a reply" is sb_rx_awaiting(): an opener that has sent blocks
 * for input. That event moves the profile back at once (it signals the stack
 * task); everything else is decided on the tick. The profile follows
 * conversations, not packets: at most one command per tick plus one per
 * conversation that starts during a bulk stream.
 */
#define SB_RX_BUSY_FRAMES 100u                       /* per tick: 2000 frames/s */
#define SB_RX_AWAIT_TICKS (300u / NETSTACK_TICK_MS)  /* a conversation is over after this long */

static void sb_netdev_profile_sync(struct SbStackCtx *ctx)
{
    UBYTE want = ctx->root->rxProfileStated;

    if (!ctx->started || ctx->root->rxAwaitSig == 0 || ctx->rxProfileInFlight ||
        ctx->rxProfileSent == want)
        return;

    ctx->rxProfileBuf.ndrp_Profile = want;
    ctx->rxProfileBuf.ndrp_Reserved = 0;
    ctx->rxProfileSent = want;
    ctx->rxProfileInFlight = TRUE;
    sb_netdev_send(&ctx->rxProfileIO, NETDEV_CMD_SET_RX_PROFILE, &ctx->rxProfileBuf,
                   sizeof(ctx->rxProfileBuf));
}

void sb_netdev_profile_decide(struct SbStackCtx *ctx)
{
    struct SocketBase *root = ctx->root;

    if (root->rxAwaitSig == 0)
        return;

    BOOL awaiting = (ULONG)(root->stackTicks - root->rxAwaitTick) < SB_RX_AWAIT_TICKS;
    root->rxProfileStated = (ctx->rxWasBusy && !awaiting) ? NDRP_THROUGHPUT : NDRP_LATENCY;
    sb_netdev_profile_sync(ctx);
}

void sb_netdev_profile_tick(struct SbStackCtx *ctx)
{
    if (ctx->root->rxAwaitSig == 0)
        return; /* no netdev interface with the capability: ndi is not even ours */

    ULONG frames = ctx->ndi.ndi_RxFrames;

    ctx->rxWasBusy = (ULONG)(frames - ctx->rxFramesPrev) >= SB_RX_BUSY_FRAMES;
    ctx->rxFramesPrev = frames;
    sb_netdev_profile_decide(ctx);
}

void sb_netdev_profile_reply(struct SbStackCtx *ctx)
{
    if (!ctx->rxProfileInFlight || CheckIO((struct IORequest *)&ctx->rxProfileIO) == NULL)
        return;
    WaitIO((struct IORequest *)&ctx->rxProfileIO);
    ctx->rxProfileInFlight = FALSE;
    sb_netdev_profile_sync(ctx); /* the decision may have moved on meanwhile */
}

void sb_netdev_profile_drain(struct SbStackCtx *ctx)
{
    ctx->root->rxAwaitSig = 0; /* no more wake-ups from openers */
    if (ctx->rxProfileInFlight)
    {
        AbortIO((struct IORequest *)&ctx->rxProfileIO);
        WaitIO((struct IORequest *)&ctx->rxProfileIO);
        ctx->rxProfileInFlight = FALSE;
    }
}

/* START has succeeded. A driver without the capability is never sent the command. */
static void sb_netdev_profile_start(struct SbStackCtx *ctx)
{
    struct SocketBase *root = ctx->root;

    ctx->rxProfileInFlight = FALSE;
    ctx->rxProfileSent = NDRP_UNSTATED; /* what the driver is on before the first command */
    ctx->rxWasBusy = FALSE;
    ctx->rxFramesPrev = ctx->ndi.ndi_RxFrames;
    root->rxProfileStated = NDRP_LATENCY;
    root->rxAwaitTick = root->stackTicks - SB_RX_AWAIT_TICKS;

    if (!(ctx->ndi.ndi_Caps.ndc_Features & NDCF_RX_PROFILE) || ctx->rxAwaitSigOwned == 0)
        return;

    /* a second request on the unit devIO has open: same device, unit and reply port */
    ctx->rxProfileIO = *ctx->devIO;
    ctx->rxProfileIO.io_Message.mn_Length = sizeof(ctx->rxProfileIO);
    root->rxAwaitSig = ctx->rxAwaitSigOwned;
    sb_netdev_profile_sync(ctx);
}

LONG sb_netdev_up(struct SbStackCtx *ctx, const struct NetCtlIfConfig *nif,
                  LONG *aux)
{
    struct NetDevAttach att; /* lives across one synchronous ATTACH DoIO only */
    memset(&att, 0, sizeof(att));
    att.nda_AbiVersion = NETDEV_ABI_VERSION;
    att.nda_RxHoldReq = netdevif_rx_hold_budget();
    att.nda_RxBatch = netdevif_rx_batch();
    att.nda_MtuReq = (nif->nif_Flags & NETCTL_IFF_HAS_MTU) ? (UWORD)nif->nif_Mtu : 0;
    att.nda_StackCtx = &ctx->ndi;
    att.nda_StackOps = netdevif_stack_ops();

    BYTE err = sb_netdev_cmd(ctx->devIO, NETDEV_CMD_ATTACH, &att, sizeof(att));
    if (err != 0)
    {
        SB_LOG(NS_LOG_ERR, "%s: netdev ATTACH failed (error %ld)", nif->nif_Name, (LONG)err);
        *aux = err;
        sb_if_down(ctx);
        return NETCTL_ERR_DEVICE;
    }
    ctx->attached = TRUE;

    /* HARDWAREADDRESS:
     * ndc_Mac was reported at ATTACH, so the netif's copy is patched here.
     * A driver without SET_MAC keeps its own address: that is a warning,
     * not a failed interface. */
    if (nif->nif_Flags & NETCTL_IFF_HAS_HWADDR)
    {
        UBYTE mac[6];
        memcpy(mac, nif->nif_HwAddr, sizeof(mac));
        err = sb_netdev_cmd(ctx->devIO, NETDEV_CMD_SET_MAC, mac, sizeof(mac));
        if (err == 0)
            memcpy(att.nda_Caps.ndc_Mac, mac, sizeof(mac));
        else
            SB_LOG(NS_LOG_WARNING, "%s: HARDWAREADDRESS not applied (driver error %ld)",
                   nif->nif_Name, (LONG)err);
    }
    const UBYTE *mac = att.nda_Caps.ndc_Mac;
    SB_LOG(NS_LOG_INFO, "%s: station address %02lx:%02lx:%02lx:%02lx:%02lx:%02lx%s",
           nif->nif_Name, (ULONG)mac[0], (ULONG)mac[1], (ULONG)mac[2], (ULONG)mac[3],
           (ULONG)mac[4], (ULONG)mac[5],
           ((nif->nif_Flags & NETCTL_IFF_HAS_HWADDR) &&
            memcmp(mac, nif->nif_HwAddr, 6) == 0) ? " (HARDWAREADDRESS)" : "");

    if (netdevif_create(&ctx->ndi, att.nda_DrvCtx, att.nda_DrvOps, &att.nda_Caps) != 0)
    {
        SB_LOG(NS_LOG_ERR, "%s: out of memory creating the interface", nif->nif_Name);
        sb_if_down(ctx);
        return NETCTL_ERR_NOMEM;
    }
    ctx->created = TRUE;
    sb_if_identify(ctx, nif); /* the driver may report link from here on */

    sb_if_configure(ctx, nif);

    err = sb_netdev_cmd(ctx->devIO, NETDEV_CMD_START, NULL, 0);
    if (err != 0)
    {
        SB_LOG(NS_LOG_ERR, "%s: netdev START failed (error %ld)", nif->nif_Name, (LONG)err);
        *aux = err;
        sb_if_down(ctx);
        return NETCTL_ERR_DEVICE;
    }
    ctx->started = TRUE;
    sb_netdev_profile_start(ctx);

    sb_if_services(ctx, nif);
    return NETCTL_OK;
}

void sb_netdev_down(struct SbStackCtx *ctx)
{
    Kprintf("[bsdsocket] %s: started %ld, attached %ld\n", __func__, (LONG)ctx->started, (LONG)ctx->attached);
    sb_mdns_stop(); /* unpublish before the netif goes away */
    if (ctx->started)
    {
        netstack_lock();
        dhcp_release_and_stop(&ctx->ndi.ndi_Base.nib_Netif);
        netif_set_down(&ctx->ndi.ndi_Base.nib_Netif);
        netstack_unlock();
        sb_netdev_profile_drain(ctx);
        sb_netdev_cmd(ctx->devIO, NETDEV_CMD_STOP, NULL, 0);
        ctx->started = FALSE;
    }
    if (ctx->created)
    {
        netdevif_destroy(&ctx->ndi);
        ctx->created = FALSE;
    }
    if (ctx->attached)
    {
        if (sb_netdev_cmd(ctx->devIO, NETDEV_CMD_DETACH, NULL, 0) != 0)
            SB_LOG(NS_LOG_WARNING, "%s: netdev DETACH failed, driver RX buffers may be leaked",
                   ctx->ndi.ndi_Base.nib_Name);
        ctx->attached = FALSE;
    }
    /* the device close itself belongs to sb_if_down (common to both backends) */
}

/* NIC-stats poll: one cycle per second, asynchronous for the reasons given at
 * sb_netdev_send. The replies are harvested from devPort — GET_STATS, then
 * GET_LINK, then one brief locked publish into the root cache. */
void sb_netdev_stats_kick(struct SbStackCtx *ctx)
{
    if (!ctx->started || ctx->statsPhase != 0)
        return; /* previous cycle still in flight: skip this second */
    sb_netdev_send(ctx->devIO, NETDEV_CMD_GET_STATS, &ctx->statsBuf, sizeof(ctx->statsBuf));
    ctx->statsPhase = 1;
}

void sb_netdev_stats_reply(struct SbStackCtx *ctx)
{
    if (ctx->statsPhase == 0 || CheckIO((struct IORequest *)ctx->devIO) == NULL)
        return;
    BYTE err = WaitIO((struct IORequest *)ctx->devIO);

    if (ctx->statsPhase == 1 && err == 0)
    {
        sb_netdev_send(ctx->devIO, NETDEV_CMD_GET_LINK, &ctx->linkBuf, sizeof(ctx->linkBuf));
        ctx->statsPhase = 2;
        return;
    }
    if (ctx->statsPhase == 2 && err == 0)
    {
        netstack_lock();
        ctx->root->netStats = ctx->statsBuf;
        ctx->root->netLink = ctx->linkBuf;
        ctx->root->netStatsValid = TRUE;
        netstack_unlock();
    }
    ctx->statsPhase = 0; /* cycle done (or failed): idle until the next kick */
}

/* Push a pending multicast RX-filter change (raised by the lwIP igmp_mac_filter
 * hook) to the driver. Off-lock by construction: NETDEV_CMD_SET_RXFILTER is
 * serviced on the driver unit task, which takes the core lock in its RX path,
 * so issuing it under the lock would deadlock. Reuses devIO, so it runs only
 * when the async stats cycle is idle; a set left dirty is retried next tick. */
void sb_netdev_rxfilter_sync(struct SbStackCtx *ctx)
{
    if (!ctx->started || ctx->statsPhase != 0 || !ctx->ndi.ndi_Base.nib_RxFilterDirty)
        return;

    UWORD overflow;
    UWORD count = netifbase_mcast_snapshot(&ctx->ndi.ndi_Base,
                                           ctx->rxFilterMacs, &overflow);
    UWORD flags = overflow > 0 ? NDFF_ALLMULTI : 0;

    struct NetDevRxFilter filter;
    filter.ndrx_Flags = flags;
    filter.ndrx_NumMcast = count;
    filter.ndrx_McastList = (const UBYTE(*)[6])ctx->rxFilterMacs;
    Kprintf("[bsdsocket] RX filter -> flags 0x%04lx, %lu mcast\n", (ULONG)flags, (ULONG)count);
    sb_netdev_cmd(ctx->devIO, NETDEV_CMD_SET_RXFILTER, &filter, sizeof(filter));
}
