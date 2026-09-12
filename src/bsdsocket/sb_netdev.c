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

    if (netdevif_create(&ctx->ndi, att.nda_DrvCtx, att.nda_DrvOps, &att.nda_Caps) != 0)
    {
        SB_LOG(NS_LOG_ERR, "%s: out of memory creating the interface", nif->nif_Name);
        sb_if_down(ctx);
        return NETCTL_ERR_NOMEM;
    }
    ctx->created = TRUE;

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

/* NIC-stats poll, asynchronous: the tick loop must never block on the driver
 * unit task (a DoIO here would stall lwIP timer servicing by the driver's
 * round-trip — worst exactly when the link is busy), and a netdev command must
 * never be issued under netstack_lock (the unit task takes that lock in its RX
 * path — deadlock). So the requests go out via SendIO and the replies are
 * harvested from devPort: GET_STATS, then GET_LINK, then one brief locked
 * publish into the root cache. */
static void sb_netdev_send(struct IOStdReq *io, UWORD cmd, APTR data, ULONG len)
{
    KprintfT("[bsdsocket] %s: cmd 0x%04lx, len %lu\n", __func__, (ULONG)cmd, len);
    io->io_Command = cmd;
    io->io_Data = data;
    io->io_Length = len;
    io->io_Actual = 0;
    SendIO((struct IORequest *)io);
}

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
