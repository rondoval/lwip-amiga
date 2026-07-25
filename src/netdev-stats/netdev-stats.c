/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * netdev-stats — loss-point counters from a netdev driver, live.
 *
 * A second opener: GET_STATS / GET_COUNTERS / GET_LINK / SET_COALESCE need no
 * ATTACH, so this runs alongside bsdsocket.library without touching the data
 * path.
 *
 *   > netdev-stats                                       one-shot summary
 *   > netdev-stats 2                                     redraw every 2 s with /s rates
 *                                                        (Ctrl-C ends)
 *   > netdev-stats COUNTERS                              the driver's own counters
 *   > netdev-stats COUNTERS 2                            ...only the ones that moved
 *   > netdev-stats RXUSECS 500 RXFRAMES 64 TXFRAMES 32   set interrupt coalescing
 *
 * ReadArgs CLI (`netdev-stats ?` prints the template): DEVICE/UNIT select the
 * driver (default genet.device unit 0); the three coalesce keywords go together.
 *
 * TWO VIEWS, and they are alternatives rather than layers, because the two
 * netdev commands behind them each answer their question in full:
 *
 *   default   NETDEV_CMD_GET_STATS — the portable summary every netdev driver
 *             answers identically: packets, bytes, errors, drops, plus the two
 *             hardware loss points (rdma = RX ring full, the drain was too
 *             slow; rbovfl = RBUF FIFO overflow, the ring did not absorb a
 *             burst) and the link state.
 *
 *   COUNTERS  NETDEV_CMD_GET_COUNTERS — whatever THIS driver keeps, every
 *             entry naming itself, so this renders a driver it knows nothing
 *             about. For genet that is the whole UniMAC MIB block: the
 *             hardware's own view of the wire, which is how you tell "the
 *             frame never arrived" from "the MAC counted it and dropped it",
 *             alongside the driver's counters and ring gauges. It restates the
 *             summary's figures too (drv_rx_packets and so on) — that is the
 *             point, since the MAC's count and the driver's count of the same
 *             thing side by side is exactly how a discrepancy shows itself.
 *             One-shot prints every counter; with an interval, only movers
 *             plus the gauges.
 *
 * Both are wrapped as a `struct view` so the sampling loop is written once and
 * knows about neither.
 */

#include <stdio.h>

#include <devices/timer.h>
#include <dos/dos.h>
#include <dos/rdargs.h>
#include <exec/io.h>
#include <exec/memory.h>
#include <exec/types.h>

#include <proto/dos.h>
#include <proto/exec.h>

#include <devices/netdev.h>

#define DEVICE_NAME "genet.device" /* default; override with DEVICE/UNIT */

#define ARG_TEMPLATE "INTERVAL/N,DEVICE/K,UNIT/K/N,COUNTERS/S,RXUSECS/K/N,RXFRAMES/K/N,TXFRAMES/K/N"
enum
{
    ARG_INTERVAL,
    ARG_DEVICE,
    ARG_UNIT,
    ARG_COUNTERS,
    ARG_RXUSECS,
    ARG_RXFRAMES,
    ARG_TXFRAMES,
    ARG_COUNT
};

/* ---------------------------------------------------------- formatting --- */

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

/* ------------------------------------------------- view: GET_STATS lines --- */

/* One direction's two lines. RX and TX print identically, so they say so. */
static void print_dir(const char *tag, const struct NetDevU64 *pkts,
                      const struct NetDevU64 *bytes, const struct NetDevU64 *errs,
                      const struct NetDevU64 *drops, const struct NetDevU64 *prevPkts,
                      const struct NetDevU64 *prevBytes, ULONG secs)
{
    char b1[22], b2[22], b3[24];

    printf("%s : pkts %s  bytes %s", tag, u64str(netdev_u64_get(pkts), b1),
           u64str(netdev_u64_get(bytes), b2));
    if (prevPkts != NULL)
        printf("  [%s Mb/s, +%lu pps]",
               mbps(netdev_u64_get(bytes) - netdev_u64_get(prevBytes), secs, b3),
               (unsigned long)((netdev_u64_get(pkts) - netdev_u64_get(prevPkts)) / (secs ? secs : 1)));
    printf("\n     errs %s  drops %s\n", u64str(netdev_u64_get(errs), b1),
           u64str(netdev_u64_get(drops), b2));
}

static void print_stats(const struct NetDevStats *s, const struct NetDevStats *prev,
                        ULONG secs, const struct NetDevLinkState *link)
{
    print_dir("rx", &s->nds_RxPackets, &s->nds_RxBytes, &s->nds_RxErrors,
              &s->nds_RxDropped, prev ? &prev->nds_RxPackets : NULL,
              prev ? &prev->nds_RxBytes : NULL, secs);
    print_dir("tx", &s->nds_TxPackets, &s->nds_TxBytes, &s->nds_TxErrors,
              &s->nds_TxDropped, prev ? &prev->nds_TxPackets : NULL,
              prev ? &prev->nds_TxBytes : NULL, secs);

    /* The two hardware loss points. Where a frame was lost inside the DRIVER
     * — pool dry, stack backpressure, TX ring full — is per-driver detail and
     * lives under COUNTERS. */
    printf("loss: rdma %lu  rbovfl %lu",
           (unsigned long)s->nds_RxOverruns, (unsigned long)s->nds_RxFifoOvfl);
    if (prev != NULL)
        printf("  [+%lu +%lu]",
               (unsigned long)(s->nds_RxOverruns - prev->nds_RxOverruns),
               (unsigned long)(s->nds_RxFifoOvfl - prev->nds_RxFifoOvfl));
    printf("\n");

    if (link != NULL)
        printf("link: %s %u Mb/s %s\n",
               (link->ndls_Flags & NDLF_UP) ? "up" : "down",
               link->ndls_SpeedMbps,
               (link->ndls_Flags & NDLF_FULL_DUPLEX) ? "FD" : "HD");
}

/* ---------------------------------------------- view: GET_COUNTERS lines --- */

/* Allocate a set sized to what the driver reports, then fill it. NULL if the
 * driver has no counters, does not implement the command, or memory is short;
 * the caller treats all three the same way. */
static struct NetDevCounterSet *counters_alloc(struct IOStdReq *io, ULONG *sizeOut)
{
    struct NetDevCounterSet probe;

    probe.ndcs_Max = 0;
    probe.ndcs_Count = 0;
    if (netdev_cmd(io, NETDEV_CMD_GET_COUNTERS, &probe, NETDEV_COUNTERSET_SIZE(0)) != 0 ||
        probe.ndcs_Count == 0)
        return NULL;

    ULONG size = NETDEV_COUNTERSET_SIZE(probe.ndcs_Count);
    struct NetDevCounterSet *set = AllocMem(size, MEMF_PUBLIC | MEMF_CLEAR);
    if (set == NULL)
        return NULL;

    set->ndcs_Max = probe.ndcs_Count;
    if (netdev_cmd(io, NETDEV_CMD_GET_COUNTERS, set, size) != 0)
    {
        FreeMem(set, size);
        return NULL;
    }

    *sizeOut = size;
    return set;
}

/* ndcs_Count is the driver's total, which may exceed what we allocated for;
 * ndcs_Max is what this buffer holds. Iterate the smaller of the two. */
static UWORD counters_filled(const struct NetDevCounterSet *set)
{
    return set->ndcs_Count < set->ndcs_Max ? set->ndcs_Count : set->ndcs_Max;
}

/* Deltas are computed per flag: a WRAP32 counter is a free-running 32-bit
 * hardware register, so subtract in 32 bits and let it wrap; anything else is
 * monotonic and subtracts in full width. Gauges are instantaneous — showing a
 * "rate" for a queue depth would be nonsense, so they print bare. */
static void print_counters(const struct NetDevCounterSet *set,
                           const unsigned long long *prev, ULONG secs)
{
    char b1[22], b2[22];
    UWORD n = counters_filled(set);

    printf("counters:\n");
    for (UWORD i = 0; i < n; i++)
    {
        const struct NetDevCounter *c = &set->ndcs_Counters[i];
        unsigned long long val = netdev_u64_get(&c->ndcn_Value);
        BOOL gauge = (c->ndcn_Flags & NDCNTF_GAUGE) != 0;

        unsigned long long delta = 0;
        if (prev != NULL && !gauge)
            delta = (c->ndcn_Flags & NDCNTF_WRAP32)
                        ? (unsigned long long)((ULONG)val - (ULONG)prev[i])
                        : val - prev[i];

        /* With an interval, silence is the useful signal: only what moved. */
        if (prev != NULL && delta == 0 && !gauge)
            continue;

        printf("  %-24s %14s", (const char *)c->ndcn_Name, u64str(val, b1));
        if (prev != NULL && !gauge)
            printf("  +%s (+%lu/s)", u64str(delta, b2),
                   (unsigned long)(delta / (secs ? secs : 1)));
        if (c->ndcn_Flags & NDCNTF_ERROR)
            printf("  <err>");
        printf("\n");
    }
}

static void counters_save(const struct NetDevCounterSet *set, unsigned long long *prev)
{
    UWORD n = counters_filled(set);

    for (UWORD i = 0; i < n; i++)
        prev[i] = netdev_u64_get(&set->ndcs_Counters[i].ndcn_Value);
}

/* ---------------------------------------------------------- the view --- */
/*
 * Both views reduce to the same two verbs — take a fresh sample, print it —
 * so the sampling loop in main() is written once and neither view leaks into
 * it. Each keeps the previous sample itself, because "previous" means a whole
 * NetDevStats to one and a parallel array of values to the other.
 */
struct view
{
    BOOL vw_Counters; /* FALSE: GET_STATS, TRUE: GET_COUNTERS */

    struct NetDevStats     vw_Stats, vw_Prev;
    struct NetDevLinkState vw_Link;
    BOOL                   vw_HaveLink;

    struct NetDevCounterSet *vw_Set;
    ULONG                    vw_SetSize;
    UWORD                    vw_Num;      /* entries vw_Set and vw_PrevVals hold */
    unsigned long long      *vw_PrevVals;
};

/* Take the first sample; FALSE means the view is unusable and has said why.
 * wantDeltas buys the extra storage a repeating view needs. */
static BOOL view_open(struct view *v, struct IOStdReq *io, BOOL counters,
                      BOOL wantDeltas, const char *devname)
{
    v->vw_Counters = counters;

    if (!counters)
    {
        BYTE err = netdev_cmd(io, NETDEV_CMD_GET_STATS, &v->vw_Stats, sizeof(v->vw_Stats));
        if (err != 0)
        {
            printf("GET_STATS failed (%d)%s\n", err,
                   err == NDERR_BADPARAMS ? " — size mismatch (rebuild driver+tool together)" : "");
            return FALSE;
        }
        v->vw_HaveLink =
            (netdev_cmd(io, NETDEV_CMD_GET_LINK, &v->vw_Link, sizeof(v->vw_Link)) == 0);
        return TRUE;
    }

    v->vw_Set = counters_alloc(io, &v->vw_SetSize);
    if (v->vw_Set == NULL)
    {
        printf("netdev-stats: %s exposes no counter list\n", devname);
        return FALSE;
    }
    v->vw_Num = v->vw_Set->ndcs_Max;

    if (wantDeltas)
    {
        v->vw_PrevVals =
            AllocMem((ULONG)v->vw_Num * sizeof(*v->vw_PrevVals), MEMF_PUBLIC | MEMF_CLEAR);
        if (v->vw_PrevVals == NULL)
            printf("(no memory for deltas — absolute values only)\n");
    }
    return TRUE;
}

/* Fresh sample, the outgoing one kept for the deltas. FALSE ends the loop. */
static BOOL view_refresh(struct view *v, struct IOStdReq *io)
{
    if (!v->vw_Counters)
    {
        v->vw_Prev = v->vw_Stats;
        if (netdev_cmd(io, NETDEV_CMD_GET_STATS, &v->vw_Stats, sizeof(v->vw_Stats)) != 0)
            return FALSE;
        v->vw_HaveLink =
            (netdev_cmd(io, NETDEV_CMD_GET_LINK, &v->vw_Link, sizeof(v->vw_Link)) == 0);
        return TRUE;
    }

    if (v->vw_PrevVals != NULL)
        counters_save(v->vw_Set, v->vw_PrevVals);
    return netdev_cmd(io, NETDEV_CMD_GET_COUNTERS, v->vw_Set, v->vw_SetSize) == 0;
}

/* withDeltas is FALSE for the first sample, which has nothing to compare to. */
static void view_show(const struct view *v, ULONG secs, BOOL withDeltas)
{
    if (v->vw_Counters)
        print_counters(v->vw_Set, withDeltas ? v->vw_PrevVals : NULL, secs);
    else
        print_stats(&v->vw_Stats, withDeltas ? &v->vw_Prev : NULL, secs,
                    v->vw_HaveLink ? &v->vw_Link : NULL);
}

/* Safe on a never-opened view, so the exit path calls it unconditionally. */
static void view_close(struct view *v)
{
    if (v->vw_PrevVals != NULL)
        FreeMem(v->vw_PrevVals, (ULONG)v->vw_Num * sizeof(*v->vw_PrevVals));
    if (v->vw_Set != NULL)
        FreeMem(v->vw_Set, v->vw_SetSize);
    v->vw_PrevVals = NULL;
    v->vw_Set = NULL;
}

/* --------------------------------------------------------- the ticker --- */

struct ticker
{
    struct MsgPort     *tk_Port;
    struct timerequest *tk_Req;
    BOOL                tk_Open;
};

static BOOL ticker_open(struct ticker *t)
{
    t->tk_Port = CreateMsgPort();
    t->tk_Req = (struct timerequest *)CreateIORequest(t->tk_Port, sizeof(struct timerequest));
    if (t->tk_Req == NULL ||
        OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_VBLANK, &t->tk_Req->tr_node, 0) != 0)
        return FALSE;

    t->tk_Open = TRUE;
    return TRUE;
}

/* TRUE on the next tick, FALSE on Ctrl-C. */
static BOOL ticker_wait(struct ticker *t, LONG secs)
{
    t->tk_Req->tr_node.io_Command = TR_ADDREQUEST;
    t->tk_Req->tr_time.tv_secs = (ULONG)secs;
    t->tk_Req->tr_time.tv_micro = 0;
    SendIO(&t->tk_Req->tr_node);

    ULONG sigs = Wait((1UL << t->tk_Port->mp_SigBit) | SIGBREAKF_CTRL_C);
    if (sigs & SIGBREAKF_CTRL_C)
    {
        AbortIO(&t->tk_Req->tr_node);
        WaitIO(&t->tk_Req->tr_node);
        return FALSE;
    }
    WaitIO(&t->tk_Req->tr_node);
    return TRUE;
}

/* Safe on a failed or never-opened ticker, so the exit path is unconditional. */
static void ticker_close(struct ticker *t)
{
    if (t->tk_Open)
        CloseDevice(&t->tk_Req->tr_node);
    if (t->tk_Req != NULL)
        DeleteIORequest(&t->tk_Req->tr_node);
    if (t->tk_Port != NULL)
        DeleteMsgPort(t->tk_Port);
    t->tk_Open = FALSE;
    t->tk_Req = NULL;
    t->tk_Port = NULL;
}

/* ----------------------------------------------------------------- main --- */

static int run_coalesce(struct IOStdReq *io, struct NetDevCoalesce *coal)
{
    BYTE err = netdev_cmd(io, NETDEV_CMD_SET_COALESCE, coal, sizeof(*coal));

    if (err != 0)
    {
        printf("SET_COALESCE failed (%d)%s\n", err,
               err == NDERR_NOTATTACHED ? " — stack not attached" : "");
        return 10;
    }
    printf("coalesce set: rx %lu us / %u frames, tx %u frames\n",
           (unsigned long)coal->ndcl_RxUsecs, coal->ndcl_RxMaxFrames, coal->ndcl_TxMaxFrames);
    return 0;
}

int main(void)
{
    int rc = 20;
    struct view view = {0};
    struct ticker tick = {0};
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
    BOOL do_counters = (argvals[ARG_COUNTERS] != 0);

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
        rc = run_coalesce(dio, &coal);
        goto out_dev;
    }

    if (!view_open(&view, dio, do_counters, interval != 0, devname))
        goto out_dev;

    view_show(&view, 0, FALSE);

    if (interval == 0)
    {
        rc = 0;
        goto out_dev;
    }

    if (!ticker_open(&tick))
    {
        printf("netdev-stats: no timer.device\n");
        goto out_dev;
    }

    while (ticker_wait(&tick, interval))
    {
        if (!view_refresh(&view, dio))
            break;
        printf("\n");
        view_show(&view, (ULONG)interval, TRUE);
    }
    rc = 0;

out_dev:
    CloseDevice((struct IORequest *)dio);
out_ports:
    ticker_close(&tick);
    view_close(&view);
    if (dio != NULL)
        DeleteIORequest((struct IORequest *)dio);
    if (devPort != NULL)
        DeleteMsgPort(devPort);
    FreeArgs(rda);
    return rc;
}
