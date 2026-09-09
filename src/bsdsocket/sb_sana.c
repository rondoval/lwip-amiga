/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * SANA-II backend, exec side: bring-up/teardown of an interface whose
 * driver speaks the classic SANA-II ABI, dispatched from sb_if_up/down,
 * plus the SANA-II flavors of the tick-driven services — the async global-
 * stats poll and the multicast add/del delta push. The datapath glue
 * (cooked-mode translation, the RX pump task, buffer callbacks) lives in
 * port/amiga/sana2_*.c.
 */

#include "sb_base.h"

#include <stddef.h> /* offsetof */

#include <exec/errors.h>
#include <exec/io.h>

#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#else
#include <proto/exec.h>
#endif

#include <debug.h>
#include <memory.h>

#include <lwip/dhcp.h>
#include <lwip/netif.h>

#include <devices/sana2.h>
#include "netstack.h"
#include "sana2_if.h"
#include "sb_mdns.h"
#include "sb_stack_priv.h"

static BYTE sb_sana_cmd(struct IOSana2Req *io, UWORD cmd)
{
    KprintfT("[bsdsocket] %s: cmd 0x%04lx\n", __func__, (ULONG)cmd);
    io->ios2_Req.io_Command = cmd;
    io->ios2_Req.io_Flags = 0;
    io->ios2_Req.io_Error = 0;
    DoIO((struct IORequest *)io);
    return io->ios2_Req.io_Error;
}

/* a usable station address: non-zero, group bit clear */
static BOOL sb_sana_mac_usable(const UBYTE *mac)
{
    if (mac[0] & 1)
        return FALSE;
    for (int i = 0; i < 6; i++)
    {
        if (mac[i] != 0)
            return TRUE;
    }
    return FALSE;
}

LONG sb_sana_up(struct SbStackCtx *ctx, const struct NetCtlIfConfig *nif,
                LONG *aux)
{
    struct IOSana2Req *io = (struct IOSana2Req *)ctx->devIO;
    /* OpenDevice consumed the tag list and left the per-opener cookie */
    APTR bufMgmt = io->ios2_BufferManagement;

    /* Wire-type gate. The two S2_DEVICEQUERY size conventions are mutually
     * exclusive: modern drivers (Poseidon's classes) REFUSE SizeAvailable
     * below the full RawMTU-bearing struct (34), while a2065-era drivers
     * fail anything ABOVE the original 30-byte layout. So ask full-size
     * first and retry with the legacy size on any error; either way the
     * fields we need end at HardwareType (offset 30 — cooked mode never
     * touches RawMTU). */
    const ULONG legacyLen = offsetof(struct Sana2DeviceQuery, RawMTU); /* 30 */
    struct Sana2DeviceQuery q;
    memset(&q, 0, sizeof(q));
    q.SizeAvailable = sizeof(q);
    io->ios2_StatData = &q;
    BYTE err = sb_sana_cmd(io, S2_DEVICEQUERY);
    if (err != 0)
    {
        memset(&q, 0, sizeof(q));
        q.SizeAvailable = legacyLen;
        err = sb_sana_cmd(io, S2_DEVICEQUERY);
    }
    io->ios2_StatData = NULL;
    if (err != 0 || q.SizeSupplied < legacyLen)
    {
        SB_LOG(NS_LOG_ERR, "%s: S2_DEVICEQUERY failed (error %ld, supplied %lu)",
               nif->nif_Name, (LONG)err, q.SizeSupplied);
        *aux = err;
        sb_if_down(ctx);
        return NETCTL_ERR_DEVICE;
    }
    if (q.HardwareType != S2WireType_Ethernet || q.AddrFieldSize != 48)
    {
        SB_LOG(NS_LOG_ERR, "%s: not a 48-bit Ethernet device (wire type %lu, %lu address bits)",
               nif->nif_Name, q.HardwareType, (ULONG)q.AddrFieldSize);
        sb_if_down(ctx);
        return NETCTL_ERR_HWTYPE;
    }

    err = sb_sana_cmd(io, S2_GETSTATIONADDRESS);
    if (err != 0)
    {
        SB_LOG(NS_LOG_ERR, "%s: S2_GETSTATIONADDRESS failed (error %ld)", nif->nif_Name,
               (LONG)err);
        *aux = err;
        sb_if_down(ctx);
        return NETCTL_ERR_DEVICE;
    }
    /* Always configure with the FACTORY address (ios2_DstAddr).
     * ios2_SrcAddr is the current address — all zeros until somebody
     * configures the unit, so feeding it back would program a
     * 00:00:00:00:00:00 MAC. */
    UBYTE mac[6];
    for (int i = 0; i < 6; i++)
        mac[i] = io->ios2_DstAddr[i];
    if (!sb_sana_mac_usable(mac))
    {
        SB_LOG(NS_LOG_ERR, "%s: driver reports no usable station address", nif->nif_Name);
        sb_if_down(ctx);
        return NETCTL_ERR_DEVICE;
    }
    SB_LOG(NS_LOG_INFO, "%s: station address %02lx:%02lx:%02lx:%02lx:%02lx:%02lx",
           nif->nif_Name, (ULONG)mac[0], (ULONG)mac[1], (ULONG)mac[2], (ULONG)mac[3],
           (ULONG)mac[4], (ULONG)mac[5]);

    /* Configure with the current station address. Already-configured (a
     * previous stack instance, or a driver that auto-configures) is fine. */
    for (int i = 0; i < 6; i++)
        io->ios2_SrcAddr[i] = mac[i];
    err = sb_sana_cmd(io, S2_CONFIGINTERFACE);
    if (err == 0)
    {
        for (int i = 0; i < 6; i++)
            mac[i] = io->ios2_SrcAddr[i]; /* the driver may have adjusted it */
    }
    else if (err == S2ERR_BAD_STATE)
    {
        Kprintf("[bsdsocket] S2_CONFIGINTERFACE: already configured\n");
    }
    else
    {
        SB_LOG(NS_LOG_ERR, "%s: S2_CONFIGINTERFACE failed (error %ld)", nif->nif_Name,
               (LONG)err);
        *aux = err;
        sb_if_down(ctx);
        return NETCTL_ERR_DEVICE;
    }

    /* BAD_STATE = already online (many drivers auto-online at configure);
     * IOERR_NOCMD tolerated for odd drivers whose configure is the switch */
    err = sb_sana_cmd(io, S2_ONLINE);
    if (err != 0 && err != S2ERR_BAD_STATE && err != IOERR_NOCMD)
    {
        SB_LOG(NS_LOG_ERR, "%s: S2_ONLINE failed (error %ld)", nif->nif_Name, (LONG)err);
        *aux = err;
        sb_if_down(ctx);
        return NETCTL_ERR_DEVICE;
    }

    /* MTU: the driver's, clamped by config; in-band VLAN costs 4 bytes of
     * payload (a tagged cooked write carries TCI+type inside ios2_DataLength,
     * which drivers check against THEIR mtu). */
    ULONG hwMtu = q.MTU;
    if (hwMtu == 0 || hwMtu > 9000)
    {
        SB_LOG(NS_LOG_WARNING, "%s: driver reports MTU %lu, using 1500", nif->nif_Name, hwMtu);
        hwMtu = 1500;
    }
    ULONG mtu = hwMtu;
    if ((nif->nif_Flags & NETCTL_IFF_HAS_MTU) && (ULONG)nif->nif_Mtu < mtu)
        mtu = (ULONG)nif->nif_Mtu;
    if (nif->nif_VlanTci >= 0)
        mtu -= 4;

    if (sana2if_create(&ctx->s2i, io->ios2_Req.io_Device, io->ios2_Req.io_Unit,
                       bufMgmt, mac, (UWORD)mtu, (UWORD)hwMtu, q.BPS,
                       nif->nif_VlanTci) != 0)
    {
        SB_LOG(NS_LOG_ERR, "%s: out of memory creating the interface", nif->nif_Name);
        sb_if_down(ctx);
        return NETCTL_ERR_NOMEM;
    }
    ctx->created = TRUE;

    /* The pump comes up BEFORE netif_set_up: a static config's set_up emits
     * a gratuitous ARP — staged write — whose SendIO needs the pump's reply
     * port stamped into the TX pool. */
    if (sana2if_pump_start(&ctx->s2i) != 0)
    {
        SB_LOG(NS_LOG_ERR, "%s: cannot start the SANA-II receive task", nif->nif_Name);
        sb_if_down(ctx);
        return NETCTL_ERR_NOMEM;
    }
    ctx->started = TRUE;

    sb_if_configure(ctx, nif);

    /* Seed the link up: the device was just onlined and SANA-II has no
     * state query — drivers with real ONLINE/OFFLINE events correct this
     * through the pump's S2_ONEVENT tracker within moments. */
    netstack_lock();
    netif_set_link_up(&ctx->s2i.s2i_Base.nib_Netif);
    netstack_unlock();

    sb_if_services(ctx, nif);
    return NETCTL_OK;
}

void sb_sana_down(struct SbStackCtx *ctx)
{
    Kprintf("[bsdsocket] %s: started %ld, created %ld\n", __func__,
            (LONG)ctx->started, (LONG)ctx->created);
    sb_mdns_stop(); /* unpublish before the netif goes away */
    if (ctx->started)
    {
        struct netif *nf = &ctx->s2i.s2i_Base.nib_Netif;
        netstack_lock();
        dhcp_release_and_stop(nf);
        netif_set_down(nf);
        ctx->s2i.s2i_TxDown = TRUE; /* no staging past this hold... */
        netstack_unlock();          /* ...which flushes the staged tail */
        sana2if_pump_stop(&ctx->s2i); /* all requests home after this */
        ctx->started = FALSE;
    }
    if (ctx->created)
    {
        sana2if_destroy(&ctx->s2i);
        ctx->created = FALSE;
    }
    if (ctx->devOpen)
    {
        /* Polite teardown; errors are meaningless here (the
         * unit may never have been configured, or TYPE=SANA2 may have been
         * forced on a non-SANA device). No CMD_FLUSH: it aborts requests
         * unit-wide, other openers' included, and ours are provably home. */
        sb_sana_cmd((struct IOSana2Req *)ctx->devIO, S2_OFFLINE);
    }
    /* the device close itself belongs to sb_if_down (common to both backends) */
}

/* Global-stats poll, single-phase: SendIO S2_GETGLOBALSTATS into s2StatsBuf,
 * harvested on devPort's signal; never a DoIO on the tick, mirroring the
 * netdev cycle. The reply maps the SANA-II counters into the neutral
 * NetDevStats/NetDevLinkState cache sb_ifquery reads — byte counters come
 * from the glue (SANA-II keeps none), link state from the live netif. */
void sb_sana_stats_kick(struct SbStackCtx *ctx)
{
    if (!ctx->started || ctx->statsPhase != 0)
        return;
    struct IOSana2Req *io = (struct IOSana2Req *)ctx->devIO;
    memset(&ctx->s2StatsBuf, 0, sizeof(ctx->s2StatsBuf));
    io->ios2_Req.io_Command = S2_GETGLOBALSTATS;
    io->ios2_Req.io_Flags = 0;
    io->ios2_Req.io_Error = 0;
    io->ios2_StatData = &ctx->s2StatsBuf;
    SendIO((struct IORequest *)io);
    ctx->statsPhase = 1;
}

void sb_sana_stats_reply(struct SbStackCtx *ctx)
{
    if (ctx->statsPhase == 0 || CheckIO((struct IORequest *)ctx->devIO) == NULL)
        return;
    BYTE err = WaitIO((struct IORequest *)ctx->devIO);
    ((struct IOSana2Req *)ctx->devIO)->ios2_StatData = NULL;

    if (err == 0)
    {
        const struct Sana2DeviceStats *ds = &ctx->s2StatsBuf;
        struct Sana2If *s2i = &ctx->s2i;

        netstack_lock();
        struct NetDevStats st;
        memset(&st, 0, sizeof(st));
        st.nds_RxPackets.ndu_Lo = ds->PacketsReceived;
        st.nds_TxPackets.ndu_Lo = ds->PacketsSent;
        st.nds_RxErrors.ndu_Lo = ds->BadData;
        st.nds_RxOverruns = ds->Overruns;
        st.nds_RxBytes.ndu_Hi = s2i->s2i_RxBytesHi;
        st.nds_RxBytes.ndu_Lo = s2i->s2i_RxBytesLo;
        st.nds_TxBytes.ndu_Hi = s2i->s2i_TxBytesHi;
        st.nds_TxBytes.ndu_Lo = s2i->s2i_TxBytesLo;
        st.nds_TxErrors.ndu_Lo = s2i->s2i_TxErrors;
        st.nds_TxDropped.ndu_Lo = s2i->s2i_TxDrops;
        st.nds_RxDropped.ndu_Lo = s2i->s2i_RxNoMem + s2i->s2i_RxErrors +
                                  s2i->s2i_RxCsumBad;

        struct NetDevLinkState link;
        link.ndls_Flags =
            netif_is_link_up(&s2i->s2i_Base.nib_Netif) ? NDLF_UP : 0;
        link.ndls_SpeedMbps = (UWORD)(s2i->s2i_Bps / 1000000);

        ctx->root->netStats = st;
        ctx->root->netLink = link;
        ctx->root->netStatsValid = TRUE;
        netstack_unlock();
    }
    ctx->statsPhase = 0; /* cycle done (or failed): idle until the next kick */
}

/* Push pending multicast joins/leaves as S2_ADD/DELMULTICASTADDRESS deltas
 * against the shadow of what was last programmed. Off-lock (DoIO per
 * address), gated on the stats cycle owning devIO, like the netdev push.
 * SANA-II has no all-multi command: overflow past NIB_MCAST_MAX is counted
 * by the base and those groups simply stay unfiltered-out (the stack still
 * drops them in software). IOERR_NOCMD latches the whole feature off. */
static BOOL sb_sana_mac_in(const UBYTE list[][6], UWORD count, const UBYTE *mac)
{
    for (UWORD i = 0; i < count; i++)
    {
        BOOL eq = TRUE;
        for (int b = 0; b < 6 && eq; b++)
            eq = list[i][b] == mac[b];
        if (eq)
            return TRUE;
    }
    return FALSE;
}

static BYTE sb_sana_mcast_cmd(struct SbStackCtx *ctx, UWORD cmd, const UBYTE *mac)
{
    struct IOSana2Req *io = (struct IOSana2Req *)ctx->devIO;
    for (int i = 0; i < 6; i++)
    {
        io->ios2_SrcAddr[i] = mac[i]; /* single address; DstAddr mirrors it
                                         for range-style implementations */
        io->ios2_DstAddr[i] = mac[i];
    }
    return sb_sana_cmd(io, cmd);
}

void sb_sana_mcast_sync(struct SbStackCtx *ctx)
{
    if (!ctx->started || ctx->statsPhase != 0 || ctx->sanaMcastUnsupported ||
        !ctx->s2i.s2i_Base.nib_RxFilterDirty)
        return;

    UWORD overflow;
    UWORD count = netifbase_mcast_snapshot(&ctx->s2i.s2i_Base,
                                           ctx->rxFilterMacs, &overflow);
    (void)overflow;

    for (UWORD i = 0; i < ctx->sanaShadowCount; i++)
    {
        if (!sb_sana_mac_in(ctx->rxFilterMacs, count, ctx->sanaShadow[i]))
        {
            BYTE err = sb_sana_mcast_cmd(ctx, S2_DELMULTICASTADDRESS,
                                         ctx->sanaShadow[i]);
            if (err == IOERR_NOCMD)
                goto unsupported;
        }
    }
    for (UWORD i = 0; i < count; i++)
    {
        if (!sb_sana_mac_in(ctx->sanaShadow, ctx->sanaShadowCount,
                            ctx->rxFilterMacs[i]))
        {
            BYTE err = sb_sana_mcast_cmd(ctx, S2_ADDMULTICASTADDRESS,
                                         ctx->rxFilterMacs[i]);
            if (err == IOERR_NOCMD)
                goto unsupported;
            if (err != 0)
                SB_LOG(NS_LOG_WARNING, "%s: S2_ADDMULTICASTADDRESS failed (error %ld)",
                       ctx->s2i.s2i_Base.nib_Name, (LONG)err);
        }
    }

    for (UWORD i = 0; i < count; i++)
        for (int b = 0; b < 6; b++)
            ctx->sanaShadow[i][b] = ctx->rxFilterMacs[i][b];
    ctx->sanaShadowCount = count;
    Kprintf("[bsdsocket] SANA-II mcast filter -> %lu group MACs\n", (ULONG)count);
    return;

unsupported:
    /* the driver receives all multicast or none — either way, stop asking */
    SB_LOG(NS_LOG_WARNING, "%s: driver has no multicast filter commands, relying on its defaults",
           ctx->s2i.s2i_Base.nib_Name);
    ctx->sanaMcastUnsupported = TRUE;
}
