/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The stack task: owns netstack time/timers and the netdev interface.
 *
 * Started under root->openLock by the first OpenLibrary(). It is a DOS
 * Process, not a bare Task: it reads ENV:netstack.prefs, and the netdev
 * attach path runs driver open code on this context, which may do DOS
 * I/O too (genet reads ENV:genet.prefs). Initializes the netstack
 * (timer.device EClock), attaches the configured network driver over the
 * netdev ABI, brings the interface up (DHCP by default, static when
 * configured), then ticks lwIP timeouts every 100 ms until told to quit
 * (library expunge).
 *
 * No NIC is not fatal: the stack still runs with just the loopback
 * interface.
 */

#include "sb_base.h"

#include <dos/dos.h>
#include <dos/dostags.h>
#include <exec/io.h>

#ifdef __INTELLISENSE__
#include <clib/dos_protos.h>
#else
#include <proto/dos.h>
#endif

#include <debug.h>

#include <lwip/dhcp.h>
#include <lwip/dns.h>
#include <lwip/netif.h>

#include <netdev.h>
#include "netdev_if.h"
#include "netstack.h"

#define SB_STACK_TICK_US 100000

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
    BOOL started;

    /* async NIC-stats poll: devIO cycles GET_STATS -> GET_LINK via SendIO so
     * the 100 ms tick never blocks on the driver unit task; results publish
     * into the root cache when the GET_LINK reply lands */
    UBYTE statsPhase; /* 0 idle, 1 GET_STATS out, 2 GET_LINK out */
    struct NetDevStats statsBuf;
    struct NetDevLinkState linkBuf;
};

/* one instance; the library is a singleton and so is the stack */
static struct SbStackCtx sb_stack;

/* the node-name part of an OpenDevice path ("networks/genet.device" ->
 * "genet.device") — resident/expansion modules register under it */
static const char *sb_dev_basename(const char *name)
{
    const char *base = name;
    for (const char *p = name; *p != '\0'; p++)
    {
        if (*p == '/' || *p == ':')
            base = p + 1;
    }
    return base;
}

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

static void sb_netdev_up(struct SbStackCtx *ctx)
{
    const struct SbNetConfig *cfg = &ctx->root->netCfg;

    ctx->devPort = CreateMsgPort();
    ctx->devIO = (struct IOStdReq *)CreateIORequest(ctx->devPort, sizeof(struct IOStdReq));
    if (ctx->devIO == NULL)
        return;

    /* the path form loads from DEVS: by convention (DEVS:Networks/...);
     * a resident/expansion module registers under the bare node name, so
     * retry with the basename before giving up */
    if (OpenDevice((CONST_STRPTR)cfg->cfg_Device, cfg->cfg_Unit,
                   (struct IORequest *)ctx->devIO, 0) != 0)
    {
        const char *base = sb_dev_basename(cfg->cfg_Device);
        if (base == cfg->cfg_Device ||
            OpenDevice((CONST_STRPTR)base, cfg->cfg_Unit,
                       (struct IORequest *)ctx->devIO, 0) != 0)
        {
            Kprintf("[bsdsocket] no %s unit %ld — loopback only\n",
                    cfg->cfg_Device, cfg->cfg_Unit);
            return;
        }
    }
    ctx->devOpen = TRUE;

    static struct NetDevAttach att; /* library data space; used once */
    for (ULONG i = 0; i < sizeof(att); i++)
        ((UBYTE *)&att)[i] = 0;
    att.nda_AbiVersion = NETDEV_ABI_VERSION;
    att.nda_RxHoldReq = netdevif_rx_hold_budget();
    att.nda_MtuReq = 0; /* driver default MTU (no MTU prefs key) */
    att.nda_StackCtx = &ctx->ndi;
    att.nda_StackOps = netdevif_stack_ops();

    BYTE err = sb_netdev_cmd(ctx->devIO, NETDEV_CMD_ATTACH, &att, sizeof(att));
    if (err != 0)
    {
        Kprintf("[bsdsocket] netdev ATTACH failed (%ld)\n", (LONG)err);
        return;
    }
    ctx->attached = TRUE;

    if (netdevif_create(&ctx->ndi, att.nda_DrvCtx, att.nda_DrvOps, &att.nda_Caps) != 0)
    {
        Kprintf("[bsdsocket] netdevif_create failed\n");
        return;
    }

    /* in-band 802.1Q VID (-1 = untagged); read per-packet by the VLAN hooks */
    ctx->ndi.ndi_VlanTci = cfg->cfg_VlanTci;
    if (cfg->cfg_VlanTci >= 0)
        Kprintf("[bsdsocket] VLAN enabled: vid %ld pcp %ld\n",
                (LONG)(cfg->cfg_VlanTci & 0xFFF), (LONG)((cfg->cfg_VlanTci >> 13) & 7));

    netstack_lock();
    netif_set_default(&ctx->ndi.ndi_Netif);
    netif_set_hostname(&ctx->ndi.ndi_Netif, ctx->root->netCfg.cfg_Hostname);
    if (!cfg->cfg_Dhcp)
        netif_set_addr(&ctx->ndi.ndi_Netif, &cfg->cfg_Addr, &cfg->cfg_Mask,
                       &cfg->cfg_Gateway);
    netif_set_up(&ctx->ndi.ndi_Netif);
    netstack_unlock();

    err = sb_netdev_cmd(ctx->devIO, NETDEV_CMD_START, NULL, 0);
    if (err != 0)
    {
        Kprintf("[bsdsocket] netdev START failed (%ld)\n", (LONG)err);
        return;
    }
    ctx->started = TRUE;

    netstack_lock();
    if (cfg->cfg_Dhcp)
    {
        dhcp_start(&ctx->ndi.ndi_Netif);
    }
    else
    {
        for (u8_t i = 0; i < 2; i++)
        {
            if (!ip4_addr_isany_val(cfg->cfg_Dns[i]))
                dns_setserver(i, &cfg->cfg_Dns[i]);
        }
    }
    netstack_unlock();
    if (cfg->cfg_Dhcp)
        Kprintf("[bsdsocket] interface up, DHCP running\n");
    else
        Kprintf("[bsdsocket] interface up, static %s\n",
                ip4addr_ntoa(&cfg->cfg_Addr));
}

static void sb_netdev_down(struct SbStackCtx *ctx)
{
    Kprintf("[bsdsocket] %s: started %ld, attached %ld\n", __func__, (LONG)ctx->started, (LONG)ctx->attached);
    if (ctx->started)
    {
        netstack_lock();
        dhcp_release_and_stop(&ctx->ndi.ndi_Netif);
        netif_set_down(&ctx->ndi.ndi_Netif);
        netstack_unlock();
        sb_netdev_cmd(ctx->devIO, NETDEV_CMD_STOP, NULL, 0);
        ctx->started = FALSE;
    }
    if (ctx->attached)
    {
        netdevif_destroy(&ctx->ndi);
        if (sb_netdev_cmd(ctx->devIO, NETDEV_CMD_DETACH, NULL, 0) != 0)
            Kprintf("[bsdsocket] netdev DETACH failed — RX buffers leaked?\n");
        ctx->attached = FALSE;
    }
    if (ctx->devOpen)
    {
        CloseDevice((struct IORequest *)ctx->devIO);
        ctx->devOpen = FALSE;
    }
    if (ctx->devIO != NULL)
    {
        DeleteIORequest((struct IORequest *)ctx->devIO);
        ctx->devIO = NULL;
    }
    if (ctx->devPort != NULL)
    {
        DeleteMsgPort(ctx->devPort);
        ctx->devPort = NULL;
    }
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

static void sb_stats_kick(struct SbStackCtx *ctx)
{
    if (!ctx->started || ctx->statsPhase != 0)
        return; /* previous cycle still in flight: skip this second */
    sb_netdev_send(ctx->devIO, NETDEV_CMD_GET_STATS, &ctx->statsBuf, sizeof(ctx->statsBuf));
    ctx->statsPhase = 1;
}

static void sb_stats_reply(struct SbStackCtx *ctx)
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

/* reclaim a stats request still in flight before devIO is reused (STOP/DETACH) */
static void sb_stats_drain(struct SbStackCtx *ctx)
{
    if (ctx->statsPhase == 0)
        return;
    AbortIO((struct IORequest *)ctx->devIO);
    WaitIO((struct IORequest *)ctx->devIO);
    ctx->statsPhase = 0;
}

static void SbStackTask(void)
{
    struct SbStackCtx *ctx = &sb_stack;
    struct Task *parent = ctx->parent;

    Kprintf("[bsdsocket] %s: starting\n", __func__);
    struct MsgPort *timerPort = CreateMsgPort();
    struct timerequest *tick =
        (struct timerequest *)CreateIORequest(timerPort, sizeof(struct timerequest));

    if (tick == NULL ||
        OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_MICROHZ, &tick->tr_node, 0) != 0)
    {
        Kprintf("[bsdsocket] stack task: no timer.device\n");
        ctx->startResult = -1;
        Signal(parent, SIGBREAKF_CTRL_F);
        goto out;
    }

    netstack_init(tick->tr_node.io_Device);
    sb_config_load(&ctx->root->netCfg);
    /* seed the resolver search domain from prefs via the LVO that owns the
     * field's truncation contract; apps may override it later the same way */
    bsd_SetDefaultDomainName((STRPTR)ctx->root->netCfg.cfg_Domain, ctx->root);
    /* anchor the loaded code base. An Emu68 [WP-HIT]/[WILD-WR] reports a raw
     * runtime 68k PC; with a known symbol's runtime address here
     * the load base falls out (base = anchor_runtime - anchor_link_offset from
     * m68k-amigaos-nm bsdsocket.library), so any PC in this binary resolves to a
     * symbol offline. Three anchors bracket the code (netstack / bsdsocket /
     * resolver); equal derived bases confirm a single code hunk. */
    Kprintf("[bsdsocket] code anchors: netstack_init@0x%08lx sb_config_load@0x%08lx"
            " bsd_SetDefaultDomainName@0x%08lx\n",
            (ULONG)netstack_init, (ULONG)sb_config_load,
            (ULONG)bsd_SetDefaultDomainName);
    sb_netdev_up(ctx);
    /* stamp interface-start time for IFQ_LastStart (UNIT_MICROHZ answers
     * TR_GETSYSTIME); harmless if no NIC came up */
    if (ctx->started)
    {
        tick->tr_node.io_Command = TR_GETSYSTIME;
        DoIO(&tick->tr_node);
        ctx->root->netLastStart.tv_secs = tick->tr_time.tv_secs;
        ctx->root->netLastStart.tv_micro = tick->tr_time.tv_micro;
    }

    ctx->startResult = 0;
    ctx->root->stackTask = FindTask(NULL);
    Signal(parent, SIGBREAKF_CTRL_F);

    tick->tr_node.io_Command = TR_ADDREQUEST;
    tick->tr_time.tv_secs = 0;
    tick->tr_time.tv_micro = SB_STACK_TICK_US;
    SendIO(&tick->tr_node);

    ULONG statTick = 0;
    ULONG devSig = ctx->devOpen ? (1UL << ctx->devPort->mp_SigBit) : 0;
    for (;;)
    {
        ULONG sigs = Wait((1UL << timerPort->mp_SigBit) | devSig | SIGBREAKF_CTRL_C);

        if (sigs & devSig)
            sb_stats_reply(ctx); /* harvest GET_STATS/GET_LINK, publish cache */

        if (sigs & (1UL << timerPort->mp_SigBit))
        {
            if (CheckIO(&tick->tr_node))
                WaitIO(&tick->tr_node);
            netstack_tick();
            tick->tr_node.io_Command = TR_ADDREQUEST;
            tick->tr_time.tv_secs = 0;
            tick->tr_time.tv_micro = SB_STACK_TICK_US;
            SendIO(&tick->tr_node);

            /* refresh the NIC stats cache once per second (10 * 100 ms);
             * fire-and-forget — the reply lands via devSig above */
            if (++statTick >= 10)
            {
                statTick = 0;
                sb_stats_kick(ctx);
            }
        }

        if (sigs & SIGBREAKF_CTRL_C)
        {
            AbortIO(&tick->tr_node);
            WaitIO(&tick->tr_node);
            break;
        }
    }

    sb_stats_drain(ctx); /* devIO must be idle before STOP/DETACH reuse it */
    sb_netdev_down(ctx);
    CloseDevice(&tick->tr_node);

out:
    if (tick != NULL)
        DeleteIORequest(&tick->tr_node);
    if (timerPort != NULL)
        DeleteMsgPort(timerPort);

    Kprintf("[bsdsocket] %s: exiting\n", __func__);
    ctx->root->stackTask = NULL;
    Signal(parent, SIGBREAKF_CTRL_F);
}

#define SB_STACK_STACK_BYTES 32768

LONG sb_stack_start(struct SocketBase *root)
{
    Kprintf("[bsdsocket] %s: root 0x%08lx\n", __func__, (ULONG)root);
    struct SbStackCtx *ctx = &sb_stack;

    for (ULONG i = 0; i < sizeof(*ctx); i++)
        ((UBYTE *)ctx)[i] = 0;
    ctx->root = root;
    ctx->parent = FindTask(NULL);
    ctx->startResult = -1;

    /* a Process, not a Task: driver open code runs on this context and may
     * do DOS I/O (genet reads ENV:genet.prefs) */
    struct DosLibrary *DOSBase =
        (struct DosLibrary *)OpenLibrary((CONST_STRPTR) "dos.library", 36);
    if (DOSBase == NULL)
        return -1;

    SetSignal(0UL, SIGBREAKF_CTRL_F);
    /* Priority must sit above the dynamic-scheduler managed band: Executive
     * reprioritizes pri <= 5, which starves this task under app CPU bursts
     * and stalls every lwIP timer. Same tier as the genet/nvme unit tasks. */
    struct Process *proc = CreateNewProcTags(
        NP_Entry, (ULONG)SbStackTask,
        NP_Name, (ULONG) "bsdsocket.library stack",
        NP_Priority, 10,
        NP_StackSize, SB_STACK_STACK_BYTES,
        TAG_DONE);
    CloseLibrary((struct Library *)DOSBase);
    if (proc == NULL)
        return -1;

    Wait(SIGBREAKF_CTRL_F);
    return ctx->startResult;
}

void sb_stack_stop(struct SocketBase *root)
{
    Kprintf("[bsdsocket] %s: stackTask 0x%08lx\n", __func__, (ULONG)root->stackTask);
    if (root->stackTask == NULL)
        return;

    SetSignal(0UL, SIGBREAKF_CTRL_F);
    Signal(root->stackTask, SIGBREAKF_CTRL_C);
    while (root->stackTask != NULL)
        Wait(SIGBREAKF_CTRL_F);
}
