/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The stack task: owns netstack time/timers and the netdev interface.
 *
 * Started under root->openLock by the first OpenLibrary(). It is a DOS
 * Process, not a bare Task: the netdev attach path runs driver open code
 * on this context, which may do DOS I/O (genet reads ENV:genet.prefs),
 * and the planned static-config reads need DOS too. Initializes the
 * netstack (timer.device EClock), attaches genet.device over the netdev
 * ABI, brings the interface up with DHCP, then ticks lwIP timeouts every
 * 100 ms until told to quit (library expunge).
 *
 * No NIC is not fatal: the stack still runs with just the loopback
 * interface. Static configuration (env vars) is a planned follow-up —
 * DHCP-by-default is the concept's UX decision.
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
};

/* one instance; the library is a singleton and so is the stack */
static struct SbStackCtx sb_stack;

static BYTE sb_netdev_cmd(struct IOStdReq *io, UWORD cmd, APTR data, ULONG len)
{
    KprintfH("[bsdsocket] %s: cmd 0x%04lx, len %lu\n", __func__, (ULONG)cmd, len);
    io->io_Command = cmd;
    io->io_Data = data;
    io->io_Length = len;
    io->io_Actual = 0;
    DoIO((struct IORequest *)io);
    return io->io_Error;
}

static void sb_netdev_up(struct SbStackCtx *ctx)
{
    ctx->devPort = CreateMsgPort();
    ctx->devIO = (struct IOStdReq *)CreateIORequest(ctx->devPort, sizeof(struct IOStdReq));
    if (ctx->devIO == NULL)
        return;

    if (OpenDevice((CONST_STRPTR) "genet.device", 0, (struct IORequest *)ctx->devIO, 0) != 0)
    {
        Kprintf("[bsdsocket] no genet.device — loopback only\n");
        return;
    }
    ctx->devOpen = TRUE;

    static struct NetDevAttach att; /* library data space; used once */
    for (ULONG i = 0; i < sizeof(att); i++)
        ((UBYTE *)&att)[i] = 0;
    att.nda_AbiVersion = NETDEV_ABI_VERSION;
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

    netstack_lock();
    netif_set_default(&ctx->ndi.ndi_Netif);
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
    dhcp_start(&ctx->ndi.ndi_Netif);
    netstack_unlock();
    Kprintf("[bsdsocket] interface up, DHCP running\n");
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
    /* build banner: ties every log to the exact library binary */
    Kprintf("[bsdsocket] stack task up, build " __DATE__ " " __TIME__ "\n");
    sb_netdev_up(ctx);

    ctx->startResult = 0;
    ctx->root->stackTask = FindTask(NULL);
    Signal(parent, SIGBREAKF_CTRL_F);

    tick->tr_node.io_Command = TR_ADDREQUEST;
    tick->tr_time.tv_secs = 0;
    tick->tr_time.tv_micro = SB_STACK_TICK_US;
    SendIO(&tick->tr_node);

    for (;;)
    {
        ULONG sigs = Wait((1UL << timerPort->mp_SigBit) | SIGBREAKF_CTRL_C);

        if (sigs & (1UL << timerPort->mp_SigBit))
        {
            if (CheckIO(&tick->tr_node))
                WaitIO(&tick->tr_node);
            netstack_tick();
            tick->tr_node.io_Command = TR_ADDREQUEST;
            tick->tr_time.tv_secs = 0;
            tick->tr_time.tv_micro = SB_STACK_TICK_US;
            SendIO(&tick->tr_node);
        }

        if (sigs & SIGBREAKF_CTRL_C)
        {
            AbortIO(&tick->tr_node);
            WaitIO(&tick->tr_node);
            break;
        }
    }

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
    /* Priority sits above dynamic-scheduler managed bands (Executive
     * reprioritizes pri <= 5; at 5 this task was starved for ~1 s under
     * app CPU bursts, stalling every lwIP timer) — same tier as the
     * genet/nvme unit tasks. */
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
