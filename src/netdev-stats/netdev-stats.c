/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * netdev-stats — loss-point counters from a netdev driver, live.
 *
 * A second opener: GET_STATS / GET_LINK / SET_COALESCE need no ATTACH, so
 * this runs alongside bsdsocket.library without touching the data path —
 * the counter visibility that DEBUG builds get from the mib/txh prints,
 * available on release builds.
 *
 *   > netdev-stats                                       one-shot dump
 *   > netdev-stats 2                                     redraw every 2 s with /s rates
 *                                                        (Ctrl-C ends)
 *   > netdev-stats RXUSECS 500 RXFRAMES 64 TXFRAMES 32   set interrupt coalescing
 *
 * ReadArgs CLI (`netdev-stats ?` prints the template): DEVICE/UNIT select the
 * driver (default genet.device unit 0); the three coalesce keywords go together.
 *
 * The loss line names the bottleneck directly: rdma = RX ring full (drain
 * too slow), rbovfl = RBUF FIFO overflow, pooldry = free pool empty (stack
 * holds too many RX buffers), backpr = stack refused delivery.
 */

#include <stdio.h>

#include <devices/timer.h>
#include <dos/dos.h>
#include <dos/rdargs.h>
#include <exec/io.h>
#include <exec/types.h>

#include <proto/dos.h>
#include <proto/exec.h>

#include <netdev.h>

#define DEVICE_NAME "genet.device" /* default; override with DEVICE/UNIT */

#define ARG_TEMPLATE "INTERVAL/N,DEVICE/K,UNIT/K/N,RXUSECS/K/N,RXFRAMES/K/N,TXFRAMES/K/N"
enum
{
    ARG_INTERVAL,
    ARG_DEVICE,
    ARG_UNIT,
    ARG_RXUSECS,
    ARG_RXFRAMES,
    ARG_TXFRAMES,
    ARG_COUNT
};

static unsigned long long u64val(const struct NetDevU64 *v)
{
    return ((unsigned long long)v->ndu_Hi << 32) | v->ndu_Lo;
}

/* decimal without printf %llu (libnix support is not a given) */
static const char *u64str(unsigned long long v, char *buf)
{
    char *p = buf + 21;
    *--p = '\0';
    do
    {
        *--p = (char)('0' + (v % 10));
        v /= 10;
    } while (v != 0);
    return p;
}

/* "123.4" Mbit/s from a byte delta over whole seconds */
static const char *mbps(unsigned long long bytes, ULONG secs, char *buf)
{
    unsigned long long mbit_x10 = bytes * 8ULL / (secs ? secs : 1) / 100000ULL;
    sprintf(buf, "%lu.%lu", (unsigned long)(mbit_x10 / 10), (unsigned long)(mbit_x10 % 10));
    return buf;
}

static BYTE netdev_cmd(struct IOStdReq *io, UWORD cmd, APTR data, ULONG len)
{
    io->io_Command = cmd;
    io->io_Data = data;
    io->io_Length = len;
    io->io_Actual = 0;
    DoIO((struct IORequest *)io);
    return io->io_Error;
}

static void print_stats(const struct NetDevStats *s, const struct NetDevStats *prev,
                        ULONG secs, const struct NetDevLinkState *link)
{
    char b1[22], b2[22], b3[24];

    printf("rx : pkts %s  bytes %s", u64str(u64val(&s->nds_RxPackets), b1),
           u64str(u64val(&s->nds_RxBytes), b2));
    if (prev != NULL)
        printf("  [%s Mb/s, +%lu pps]",
               mbps(u64val(&s->nds_RxBytes) - u64val(&prev->nds_RxBytes), secs, b3),
               (unsigned long)((u64val(&s->nds_RxPackets) - u64val(&prev->nds_RxPackets)) / (secs ? secs : 1)));
    printf("\n     errs %s  drops %s\n", u64str(u64val(&s->nds_RxErrors), b1),
           u64str(u64val(&s->nds_RxDropped), b2));

    printf("tx : pkts %s  bytes %s", u64str(u64val(&s->nds_TxPackets), b1),
           u64str(u64val(&s->nds_TxBytes), b2));
    if (prev != NULL)
        printf("  [%s Mb/s, +%lu pps]",
               mbps(u64val(&s->nds_TxBytes) - u64val(&prev->nds_TxBytes), secs, b3),
               (unsigned long)((u64val(&s->nds_TxPackets) - u64val(&prev->nds_TxPackets)) / (secs ? secs : 1)));
    printf("\n");

    printf("loss: rdma %lu  rbovfl %lu  pooldry %lu  backpr %lu  txrej %lu  txbad %lu",
           (unsigned long)s->nds_RxOverruns, (unsigned long)s->nds_RxFifoOvfl,
           (unsigned long)s->nds_RxPoolDry, (unsigned long)s->nds_RxBackpressure,
           (unsigned long)s->nds_TxRejected, (unsigned long)s->nds_TxBad);
    if (prev != NULL)
        printf("  [+%lu +%lu +%lu +%lu +%lu +%lu]",
               (unsigned long)(s->nds_RxOverruns - prev->nds_RxOverruns),
               (unsigned long)(s->nds_RxFifoOvfl - prev->nds_RxFifoOvfl),
               (unsigned long)(s->nds_RxPoolDry - prev->nds_RxPoolDry),
               (unsigned long)(s->nds_RxBackpressure - prev->nds_RxBackpressure),
               (unsigned long)(s->nds_TxRejected - prev->nds_TxRejected),
               (unsigned long)(s->nds_TxBad - prev->nds_TxBad));
    printf("\n");

    printf("irq : rx-done %lu", (unsigned long)s->nds_IrqRx);
    if (prev != NULL)
        printf(" (+%lu/s)", (unsigned long)((s->nds_IrqRx - prev->nds_IrqRx) / (secs ? secs : 1)));
    printf("   pool free %lu", (unsigned long)s->nds_RxPoolFree);
    if (link != NULL)
        printf("   link %s %u Mb/s %s",
               (link->ndls_Flags & NDLF_UP) ? "up" : "down",
               link->ndls_SpeedMbps,
               (link->ndls_Flags & NDLF_FULL_DUPLEX) ? "FD" : "HD");
    printf("\n");
}

int main(void)
{
    int rc = 20;
    struct NetDevCoalesce coal;

    LONG argvals[ARG_COUNT] = {0};
    struct RDArgs *rda = ReadArgs((CONST_STRPTR)ARG_TEMPLATE, argvals, NULL);
    if (rda == NULL)
    {
        PrintFault(IoErr(), (CONST_STRPTR) "netdev-stats");
        return 10;
    }

    /* devname points into rda's buffers: FreeArgs only at exit */
    LONG interval = argvals[ARG_INTERVAL] ? *(LONG *)argvals[ARG_INTERVAL] : 0;
    const char *devname = argvals[ARG_DEVICE] ? (const char *)argvals[ARG_DEVICE]
                                              : DEVICE_NAME;
    LONG unit = argvals[ARG_UNIT] ? *(LONG *)argvals[ARG_UNIT] : 0;

    int ncoal = (argvals[ARG_RXUSECS] != 0) + (argvals[ARG_RXFRAMES] != 0) +
                (argvals[ARG_TXFRAMES] != 0);
    BOOL do_coalesce = (ncoal == 3);
    if (ncoal != 0 && ncoal != 3)
    {
        printf("netdev-stats: RXUSECS, RXFRAMES and TXFRAMES must be given together\n");
        FreeArgs(rda);
        return 10;
    }
    if (argvals[ARG_INTERVAL] != 0 && (interval <= 0 || do_coalesce))
    {
        printf("netdev-stats: INTERVAL must be >= 1 and cannot combine with coalescing\n");
        FreeArgs(rda);
        return 10;
    }
    if (do_coalesce)
    {
        coal.ndcl_RxUsecs = (ULONG)*(LONG *)argvals[ARG_RXUSECS];
        coal.ndcl_RxMaxFrames = (UWORD)*(LONG *)argvals[ARG_RXFRAMES];
        coal.ndcl_TxMaxFrames = (UWORD)*(LONG *)argvals[ARG_TXFRAMES];
    }

    struct MsgPort *devPort = CreateMsgPort();
    struct IOStdReq *dio = (struct IOStdReq *)CreateIORequest(devPort, sizeof(struct IOStdReq));
    if (dio == NULL ||
        OpenDevice((CONST_STRPTR)devname, (ULONG)unit, (struct IORequest *)dio, 0) != 0)
    {
        printf("netdev-stats: cannot open %s unit %ld\n", devname, (long)unit);
        goto out_ports;
    }

    if (do_coalesce)
    {
        BYTE err = netdev_cmd(dio, NETDEV_CMD_SET_COALESCE, &coal, sizeof(coal));
        if (err != 0)
            printf("SET_COALESCE failed (%d)%s\n", err,
                   err == NDERR_NOTATTACHED ? " — stack not attached" : "");
        else
            printf("coalesce set: rx %lu us / %u frames, tx %u frames\n",
                   (unsigned long)coal.ndcl_RxUsecs, coal.ndcl_RxMaxFrames, coal.ndcl_TxMaxFrames);
        rc = (err == 0) ? 0 : 10;
        goto out_dev;
    }

    struct NetDevStats stats, prevStats;
    struct NetDevLinkState link;
    BYTE err = netdev_cmd(dio, NETDEV_CMD_GET_STATS, &stats, sizeof(stats));
    if (err != 0)
    {
        printf("GET_STATS failed (%d)%s\n", err,
               err == NDERR_BADPARAMS ? " — size mismatch (rebuild driver+tool together)" : "");
        goto out_dev;
    }
    BOOL have_link = (netdev_cmd(dio, NETDEV_CMD_GET_LINK, &link, sizeof(link)) == 0);

    if (interval == 0)
    {
        print_stats(&stats, NULL, 0, have_link ? &link : NULL);
        rc = 0;
        goto out_dev;
    }

    /* interval mode: absolute counters plus per-interval rates */
    struct MsgPort *timerPort = CreateMsgPort();
    struct timerequest *tr = (struct timerequest *)CreateIORequest(timerPort, sizeof(struct timerequest));
    if (tr == NULL || OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_VBLANK, &tr->tr_node, 0) != 0)
    {
        printf("netdev-stats: no timer.device\n");
        goto out_timer_ports;
    }

    print_stats(&stats, NULL, 0, have_link ? &link : NULL);

    for (;;)
    {
        tr->tr_node.io_Command = TR_ADDREQUEST;
        tr->tr_time.tv_secs = (ULONG)interval;
        tr->tr_time.tv_micro = 0;
        SendIO(&tr->tr_node);

        ULONG sigs = Wait((1UL << timerPort->mp_SigBit) | SIGBREAKF_CTRL_C);
        if (sigs & SIGBREAKF_CTRL_C)
        {
            AbortIO(&tr->tr_node);
            WaitIO(&tr->tr_node);
            break;
        }
        WaitIO(&tr->tr_node);

        prevStats = stats;
        if (netdev_cmd(dio, NETDEV_CMD_GET_STATS, &stats, sizeof(stats)) != 0)
            break;
        have_link = (netdev_cmd(dio, NETDEV_CMD_GET_LINK, &link, sizeof(link)) == 0);

        printf("\n");
        print_stats(&stats, &prevStats, (ULONG)interval, have_link ? &link : NULL);
    }
    rc = 0;

    CloseDevice(&tr->tr_node);
out_timer_ports:
    if (tr != NULL)
        DeleteIORequest(&tr->tr_node);
    if (timerPort != NULL)
        DeleteMsgPort(timerPort);

out_dev:
    CloseDevice((struct IORequest *)dio);
out_ports:
    if (dio != NULL)
        DeleteIORequest((struct IORequest *)dio);
    if (devPort != NULL)
        DeleteMsgPort(devPort);
    FreeArgs(rda);
    return rc;
}
