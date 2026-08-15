/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The stack task: owns netstack time/timers and the netdev interface.
 *
 * Started under root->openLock by the first OpenLibrary(). It is a DOS
 * Process, not a bare Task: it reads ENV:netstack.prefs, and the netdev
 * attach path runs driver open code on this context, which may do DOS
 * I/O too (genet reads ENV:genet.prefs).
 *
 * The task boots the stack with the loopback
 * interface only, publishes the netstack_ctl.h control port, and ticks lwIP
 * timeouts every 100 ms. Network interfaces are added and removed at
 * runtime through that port (the AddNetInterface / RemoveNetInterface
 * commands drive sb_netdev_up/down here), and the stack stops through it
 * too (NetShutdown) — or via sb_stack_stop from a failed-open unwind.
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

#include <devices/netdev.h>
#include "netdev_if.h"
#include "netstack.h"
#include "sb_mdns.h"
#include "sb_netctl.h"
#include "sb_stack_priv.h"

#define SB_STACK_TICK_US 100000

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

LONG sb_netdev_up(struct SbStackCtx *ctx, const struct NetCtlIfConfig *nif,
                  LONG *aux)
{
    *aux = 0;

    ctx->devPort = CreateMsgPort();
    ctx->devIO = (struct IOStdReq *)CreateIORequest(ctx->devPort, sizeof(struct IOStdReq));
    if (ctx->devIO == NULL)
    {
        sb_netdev_down(ctx);
        return NETCTL_ERR_NOMEM;
    }

    /* the path form loads from DEVS: by convention (DEVS:Networks/...);
     * a resident/expansion module registers under the bare node name, so
     * retry with the basename before giving up */
    if (OpenDevice((CONST_STRPTR)nif->nif_Device, nif->nif_Unit,
                   (struct IORequest *)ctx->devIO, 0) != 0)
    {
        const char *base = sb_dev_basename(nif->nif_Device);
        if (base == nif->nif_Device ||
            OpenDevice((CONST_STRPTR)base, nif->nif_Unit,
                       (struct IORequest *)ctx->devIO, 0) != 0)
        {
            Kprintf("[bsdsocket] no %s unit %ld\n",
                    nif->nif_Device, nif->nif_Unit);
            *aux = ctx->devIO->io_Error;
            sb_netdev_down(ctx);
            return NETCTL_ERR_DEVICE;
        }
    }
    ctx->devOpen = TRUE;

    struct NetDevAttach att; /* lives across one synchronous ATTACH DoIO only */
    for (ULONG i = 0; i < sizeof(att); i++)
        ((UBYTE *)&att)[i] = 0;
    att.nda_AbiVersion = NETDEV_ABI_VERSION;
    att.nda_RxHoldReq = netdevif_rx_hold_budget();
    att.nda_RxBatch = netdevif_rx_batch();
    att.nda_MtuReq = (nif->nif_Flags & NETCTL_IFF_HAS_MTU) ? (UWORD)nif->nif_Mtu : 0;
    att.nda_StackCtx = &ctx->ndi;
    att.nda_StackOps = netdevif_stack_ops();

    BYTE err = sb_netdev_cmd(ctx->devIO, NETDEV_CMD_ATTACH, &att, sizeof(att));
    if (err != 0)
    {
        Kprintf("[bsdsocket] netdev ATTACH failed (%ld)\n", (LONG)err);
        *aux = err;
        sb_netdev_down(ctx);
        return NETCTL_ERR_DEVICE;
    }
    ctx->attached = TRUE;

    if (netdevif_create(&ctx->ndi, att.nda_DrvCtx, att.nda_DrvOps, &att.nda_Caps) != 0)
    {
        Kprintf("[bsdsocket] netdevif_create failed\n");
        sb_netdev_down(ctx);
        return NETCTL_ERR_NOMEM;
    }
    ctx->created = TRUE;

    /* identity: what the query LVOs and the control port know this interface
     * as; lwIP's own name stays "nd<n>" */
    struct netif *nf = &ctx->ndi.ndi_Base.nib_Netif;
    netifbase_stamp(&ctx->ndi.ndi_Base, nif, ctx->root->netCfg.cfg_Hostname);
    if (nif->nif_VlanTci >= 0)
        Kprintf("[bsdsocket] VLAN enabled: vid %ld pcp %ld\n",
                (LONG)(nif->nif_VlanTci & 0xFFF), (LONG)((nif->nif_VlanTci >> 13) & 7));

    netstack_lock();
    netif_set_default(nf);
    netif_set_hostname(nf, ctx->ndi.ndi_Base.nib_Hostname);
    if (!(nif->nif_Flags & NETCTL_IFF_DHCP))
    {
        ip4_addr_t addr, mask, gw;
        addr.addr = nif->nif_Addr;
        mask.addr = nif->nif_Mask;
        gw.addr = nif->nif_Gateway;
        netif_set_addr(nf, &addr, &mask, &gw);
    }
    netif_set_up(nf);
    netstack_unlock();

    err = sb_netdev_cmd(ctx->devIO, NETDEV_CMD_START, NULL, 0);
    if (err != 0)
    {
        Kprintf("[bsdsocket] netdev START failed (%ld)\n", (LONG)err);
        *aux = err;
        sb_netdev_down(ctx);
        return NETCTL_ERR_DEVICE;
    }
    ctx->started = TRUE;

    if (nif->nif_Flags & NETCTL_IFF_DHCP)
    {
        netstack_lock();
        dhcp_start(nf);
        netstack_unlock();
    }
    if (nif->nif_Flags & NETCTL_IFF_DHCP)
        Kprintf("[bsdsocket] interface up, DHCP running\n");
    else
        Kprintf("[bsdsocket] interface up, static %lu.%lu.%lu.%lu\n",
                (nif->nif_Addr >> 24) & 0xFF, (nif->nif_Addr >> 16) & 0xFF,
                (nif->nif_Addr >> 8) & 0xFF, nif->nif_Addr & 0xFF);

    /* mDNS last: the responder probes as soon as it is added, and it wants an
     * interface that is already up (an address is not required — it re-probes
     * itself when DHCP supplies one). */
    sb_mdns_start(nf, &ctx->root->netCfg);
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
void sb_stats_drain(struct SbStackCtx *ctx)
{
    if (ctx->statsPhase == 0)
        return;
    AbortIO((struct IORequest *)ctx->devIO);
    WaitIO((struct IORequest *)ctx->devIO);
    ctx->statsPhase = 0;
}

/* Push a pending multicast RX-filter change (raised by the lwIP igmp_mac_filter
 * hook) to the driver. Off-lock by construction: NETDEV_CMD_SET_RXFILTER is
 * serviced on the driver unit task, which takes the core lock in its RX path,
 * so issuing it under the lock would deadlock. Reuses devIO, so it runs only
 * when the async stats cycle is idle; a set left dirty is retried next tick. */
static void sb_rxfilter_sync(struct SbStackCtx *ctx)
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
    /* stack-wide DNS overrides apply from boot; sb_netctl re-applies them
     * whenever a DHCP lease lands, so explicit config beats the lease */
    netstack_lock();
    for (u8_t i = 0; i < 2; i++)
    {
        if (!ip4_addr_isany_val(ctx->root->netCfg.cfg_Dns[i]))
            dns_setserver(i, &ctx->root->netCfg.cfg_Dns[i]);
    }
    netstack_unlock();

    /* the stack boots with loopback only; interfaces are
     * added at runtime through the control port (AddNetInterface). No port
     * means no way to ever add one — treat that OOM as a failed start. */
    if (sb_netctl_start() != 0)
    {
        Kprintf("[bsdsocket] stack task: no control port\n");
        CloseDevice(&tick->tr_node);
        ctx->startResult = -1;
        Signal(parent, SIGBREAKF_CTRL_F);
        goto out;
    }
    Kprintf("[bsdsocket] stack up (loopback only) — waiting for AddNetInterface\n");

    ctx->startResult = 0;
    ctx->root->stackTask = FindTask(NULL);
    Signal(parent, SIGBREAKF_CTRL_F);

    tick->tr_node.io_Command = TR_ADDREQUEST;
    tick->tr_time.tv_secs = 0;
    tick->tr_time.tv_micro = SB_STACK_TICK_US;
    SendIO(&tick->tr_node);

    ULONG statTick = 0;
    for (;;)
    {
        /* recomputed every pass: interfaces (and their ports) now come and
         * go at runtime, so no signal may be latched across an iteration */
        ULONG devSig = ctx->devPort != NULL ? (1UL << ctx->devPort->mp_SigBit) : 0;
        ULONG mdnsSig = sb_mdns_sigmask();
        ULONG ctlSig = sb_netctl_sigmask();
        /* CTRL_E: LibClose's "last client of a pending shutdown left" wake */
        ULONG sigs = Wait((1UL << timerPort->mp_SigBit) | devSig | mdnsSig |
                          ctlSig | SIGBREAKF_CTRL_C | SIGBREAKF_CTRL_E);

        if (sigs & devSig)
            sb_stats_reply(ctx); /* harvest GET_STATS/GET_LINK, publish cache */

        if (sigs & mdnsSig)
            sb_mdns_service(); /* `mdns` add/del/list of advertised services */

        if (sigs & ctlSig)
            sb_netctl_service(ctx); /* interface add/remove, stack shutdown */

        if (sigs & (1UL << timerPort->mp_SigBit))
        {
            if (CheckIO(&tick->tr_node))
                WaitIO(&tick->tr_node);
            netstack_tick();
            tick->tr_node.io_Command = TR_ADDREQUEST;
            tick->tr_time.tv_secs = 0;
            tick->tr_time.tv_micro = SB_STACK_TICK_US;
            SendIO(&tick->tr_node);

            /* answer a parked AddNetInterface once its DHCP lease is bound */
            sb_netctl_tick(ctx);

            /* push any pending multicast filter change (devIO must be idle) */
            sb_rxfilter_sync(ctx);

            /* refresh the NIC stats cache once per second (10 * 100 ms);
             * fire-and-forget — the reply lands via devSig above */
            if (++statTick >= 10)
            {
                statTick = 0;
                sb_stats_kick(ctx);
                sb_netctl_nudge(ctx); /* re-ask shutdown holdouts to leave */
            }
        }

        /* checked every pass, not only on CTRL_E: a SHUTDOWN that arrives
         * with zero clients must complete without waiting for a wake */
        BOOL quit = (sigs & SIGBREAKF_CTRL_C) != 0 || sb_netctl_shutdown_ready(ctx);
        if (quit)
        {
            AbortIO(&tick->tr_node);
            WaitIO(&tick->tr_node);
            break;
        }
    }

    sb_netctl_stop(ctx); /* withdraw the port, answer everything owed
                            (except a parked SHUTDOWN — see below) */
    sb_stats_drain(ctx); /* devIO must be idle before STOP/DETACH reuse it */
    sb_netdev_down(ctx);
    CloseDevice(&tick->tr_node);

out:
    /* Everything the epilogue needs, captured BEFORE stackTask is cleared:
     * the moment it is, a racing OpenLibrary may restart the stack and
     * sb_stack_start re-zeroes the shared ctx — locals only from here on. */
    struct SocketBase *root = ctx->root;
    struct NetCtlMsg *shutdownMsg = ctx->pendingShutdown;
    struct Task *stopper = ctx->stopRequested ? ctx->parent : NULL;
    ctx->pendingShutdown = NULL;

    if (tick != NULL)
        DeleteIORequest(&tick->tr_node);
    if (timerPort != NULL)
        DeleteMsgPort(timerPort);

    Kprintf("[bsdsocket] %s: exiting\n", __func__);

    /* Atomically under openLock: a LibOpen must see either "shutting down"
     * (refuse) or "no stack task" (fresh start) — never a stale stackTask
     * with the flag already clear, which would hand out a child base wired
     * to a task that no longer ticks. */
    ObtainSemaphore(&root->openLock);
    root->stackTask = NULL;
    root->shuttingDown = FALSE;
    ReleaseSemaphore(&root->openLock);

    /* Exit handshake only when sb_stack_stop asked for one — it re-aims
     * ctx->parent at itself first (the startup handshake's target is the
     * starter, long gone in general). Other exits have no waiter;
     * signalling the stale starter would poke a freed Task. */
    if (stopper != NULL)
        Signal(stopper, SIGBREAKF_CTRL_F);

    /* The NetShutdown reply is this task's very LAST act, under Forbid():
     * the client calls RemLibrary the moment the reply lands, unloading the
     * seglist this code lives in. Forbid held across the final RTS (which
     * returns into dos.library's process glue, outside this seglist)
     * guarantees nothing of ours executes after the reply is visible. The
     * Forbid dies with the task (RemTask resets it). */
    if (shutdownMsg != NULL)
    {
        shutdownMsg->ncm_Result = NETCTL_OK;
        shutdownMsg->ncm_Count = 0;
        Forbid();
        ReplyMsg(&shutdownMsg->ncm_Msg);
    }
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

    /* Redirect the exit handshake at ourselves before asking for it. The
     * starter's task is what ctx->parent holds until now, and the stopper is
     * rarely the starter — expunge runs in whichever task ran low on memory.
     * Signalling the stale entry would both hang us here and poke a Task
     * structure that may already be freed. */
    sb_stack.stopRequested = TRUE;
    sb_stack.parent = FindTask(NULL);
    SetSignal(0UL, SIGBREAKF_CTRL_F);
    Signal(root->stackTask, SIGBREAKF_CTRL_C);
    while (root->stackTask != NULL)
        Wait(SIGBREAKF_CTRL_F);
}
