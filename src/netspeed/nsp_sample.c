/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * NetSpeed sampler — see nsp_sample.h.
 */

#include "nsp_sample.h"

#include <string.h>

#include <devices/timer.h>
#include <exec/lists.h>
#include <exec/nodes.h>
#include <libraries/bsdsocket.h>
#include <utility/tagitem.h>

#include <proto/socket.h>
#include <proto/timer.h>

/* --- arithmetic ----------------------------------------------------------- */

/* The monotonic EClock as one 64-bit count. */
static unsigned long long nsp_sample_now(void)
{
    struct EClockVal ev;
    ReadEClock(&ev);
    return ((unsigned long long)ev.ev_hi << 32) | ev.ev_lo;
}

/* A bsdsocket 64-bit counter as one value. */
static unsigned long long nsp_sample_quad(const SBQUAD_T *q)
{
    return ((unsigned long long)q->sbq_High << 32) | q->sbq_Low;
}

/* Saturate to 32 bits (a 1 Gb/s link cannot overflow in one tick, a
 * counter jump after a stack restart could). */
static ULONG nsp_sample_clamp(unsigned long long v)
{
    return v > 0xFFFFFFFFULL ? 0xFFFFFFFFUL : (ULONG)v;
}

/* Bytes per second over a span of EClock ticks (> 0), saturated. */
static ULONG nsp_sample_rate(unsigned long long bytes, unsigned long long ticks, ULONG freq)
{
    return nsp_sample_clamp(bytes * freq / ticks);
}

/* --- the interface set ---------------------------------------------------- */

/* Poll the stack's interface list and restart the poll countdown; TRUE when
 * the set of names changed. */
static BOOL nsp_sample_rescan(struct NspSampler *s)
{
    s->rescanIn = NSP_RESCAN_SECS / NSP_TICK_SECS;

    struct NspIfaceSet fresh;
    fresh.count = 0;
    struct List *list = ObtainInterfaceList();
    if (list != NULL)
    {
        for (struct Node *n = list->lh_Head; n->ln_Succ != NULL && fresh.count < NSP_IFACE_MAX;
             n = n->ln_Succ)
            strlcpy(fresh.names[fresh.count++], (const char *)n->ln_Name, NSP_NAME_MAX);
        ReleaseInterfaceList(list);
    }

    BOOL changed = fresh.count != s->ifaces.count;
    for (ULONG i = 0; !changed && i < fresh.count; i++)
        changed = strcmp(fresh.names[i], s->ifaces.names[i]) != 0;
    if (changed)
    {
        s->ifaces.count = fresh.count;
        for (ULONG i = 0; i < fresh.count; i++)
            strlcpy(s->ifaces.names[i], fresh.names[i], NSP_NAME_MAX);
    }
    return changed;
}

/* --- statistics ----------------------------------------------------------- */

/* No rate shown, no history for the moving average. */
static void nsp_sample_clear_stats(struct NspSampler *s)
{
    s->valid = FALSE;
    s->curRx = s->curTx = s->avgRx = s->avgTx = s->maxRx = s->maxTx = 0;
    s->histHead = 0;
    s->histCount = 0;
}

/* The first sample of a source: both the last sample and the "since Reset"
 * base start here, and there is nothing to show yet. */
static void nsp_sample_seed(struct NspSampler *s, unsigned long long rx, unsigned long long tx,
                            unsigned long long now)
{
    s->lastRx = s->baseRx = rx;
    s->lastTx = s->baseTx = tx;
    s->lastTicks = s->baseTicks = now;
    s->haveLast = TRUE;
    nsp_sample_clear_stats(s);
}

/* The average: bytes over time since the base, or across the newest
 * avgSamples samples of the history. */
static void nsp_sample_average(struct NspSampler *s, unsigned long long rxNow,
                               unsigned long long txNow, unsigned long long now)
{
    if (s->avgSamples == 0)
    {
        unsigned long long sinceBase = now - s->baseTicks; /* >= one sample > 0 */
        s->avgRx = nsp_sample_rate(rxNow - s->baseRx, sinceBase, s->eclockFreq);
        s->avgTx = nsp_sample_rate(txNow - s->baseTx, sinceBase, s->eclockFreq);
        return;
    }
    ULONG n = s->histCount < s->avgSamples ? s->histCount : s->avgSamples;
    unsigned long long rx = 0, tx = 0, ticks = 0;
    ULONG idx = s->histHead;
    for (ULONG i = 0; i < n; i++)
    {
        idx = (idx == 0 ? NSP_AVG_MAX : idx) - 1;
        rx += s->hist[idx].rx;
        tx += s->hist[idx].tx;
        ticks += s->hist[idx].ticks;
    }
    if (ticks == 0)
        return;
    s->avgRx = nsp_sample_rate(rx, ticks, s->eclockFreq);
    s->avgTx = nsp_sample_rate(tx, ticks, s->eclockFreq);
}

/* --- the public API ------------------------------------------------------- */

/* No source yet; the interface list is polled once so the menu can be built. */
void nsp_sample_init(struct NspSampler *s, ULONG eclockFreq)
{
    s->ifaces.count = 0;
    s->eclockFreq = eclockFreq;
    s->avgSamples = 0;
    s->haveLast = FALSE;
    s->haveLink = FALSE;
    s->bps = 0;
    s->state = 0;
    s->linkBytes = 0;
    nsp_sample_clear_stats(s);
    nsp_sample_rescan(s);
}

/* The window in samples, capped by the history. */
void nsp_sample_set_average(struct NspSampler *s, LONG secs)
{
    ULONG samples = secs > 0 ? (ULONG)secs / NSP_TICK_SECS : 0;
    s->avgSamples = samples < NSP_AVG_MAX ? samples : NSP_AVG_MAX;
}

/* Dropping the last sample makes the tick seed; the statistics are cleared
 * whether or not the source exists. */
BOOL nsp_sample_select(struct NspSampler *s, const char *name)
{
    s->haveLast = FALSE;
    nsp_sample_clear_stats(s);
    return (nsp_sample_tick(s, name) & NSP_TICK_FAILED) == 0;
}

/* The last sample becomes the base, so the next tick yields a rate. */
void nsp_sample_reset(struct NspSampler *s)
{
    nsp_sample_clear_stats(s);
    s->baseRx = s->lastRx;
    s->baseTx = s->lastTx;
    s->baseTicks = s->lastTicks;
}

/* Query the source, then turn the counter deltas into rates. The interface
 * list is polled when due; a failed query polls it right away, since the
 * interface is most likely gone. */
ULONG nsp_sample_tick(struct NspSampler *s, const char *name)
{
    ULONG r = 0;
    if (--s->rescanIn <= 0 && nsp_sample_rescan(s))
        r |= NSP_TICK_IFACES;

    /* unanswered tags leave their storage untouched: pre-zero everything */
    SBQUAD_T rx, tx;
    rx.sbq_High = rx.sbq_Low = 0;
    tx.sbq_High = tx.sbq_Low = 0;
    LONG bps = 0;
    LONG state = 0;
    BOOL haveLink = FALSE;

    if (name[0] == '\0')
    {
        struct TagItem tags[3];
        tags[0].ti_Tag = SBTM_GETREF(SBTC_GET_BYTES_RECEIVED);
        tags[0].ti_Data = (ULONG)&rx;
        tags[1].ti_Tag = SBTM_GETREF(SBTC_GET_BYTES_SENT);
        tags[1].ti_Data = (ULONG)&tx;
        tags[2].ti_Tag = TAG_END;
        SocketBaseTagList(tags);
        /* one interface is the whole network: show its link too */
        if (s->ifaces.count == 1)
            haveLink = QueryInterfaceTags((STRPTR)s->ifaces.names[0], IFQ_BPS, (Tag)&bps,
                                          IFQ_State, (Tag)&state, TAG_END) == 0;
    }
    else if (QueryInterfaceTags((STRPTR)name, IFQ_GetBytesIn, (Tag)&rx, IFQ_GetBytesOut, (Tag)&tx,
                                IFQ_BPS, (Tag)&bps, IFQ_State, (Tag)&state, TAG_END) != 0)
    {
        if (nsp_sample_rescan(s))
            r |= NSP_TICK_IFACES;
        return r | NSP_TICK_FAILED;
    }
    else
        haveLink = TRUE;
    s->haveLink = haveLink;
    s->bps = bps;
    s->state = state;
    s->linkBytes = haveLink && state == SM_Up && bps > 0 ? (ULONG)bps / 8 : 0;

    unsigned long long now = nsp_sample_now();
    unsigned long long rxNow = nsp_sample_quad(&rx);
    unsigned long long txNow = nsp_sample_quad(&tx);
    unsigned long long ticks = now - s->lastTicks;

    /* first sample, restarted counters (interface re-added) or no time
     * passed: nothing to divide yet */
    if (!s->haveLast || rxNow < s->lastRx || txNow < s->lastTx || ticks == 0)
    {
        nsp_sample_seed(s, rxNow, txNow, now);
        return r;
    }

    struct NspSample *d = &s->hist[s->histHead];
    d->rx = nsp_sample_clamp(rxNow - s->lastRx);
    d->tx = nsp_sample_clamp(txNow - s->lastTx);
    d->ticks = nsp_sample_clamp(ticks);
    s->histHead = (s->histHead + 1) % NSP_AVG_MAX;
    if (s->histCount < NSP_AVG_MAX)
        s->histCount++;

    s->curRx = nsp_sample_rate(d->rx, ticks, s->eclockFreq);
    s->curTx = nsp_sample_rate(d->tx, ticks, s->eclockFreq);
    if (s->curRx > s->maxRx)
        s->maxRx = s->curRx;
    if (s->curTx > s->maxTx)
        s->maxTx = s->curTx;
    nsp_sample_average(s, rxNow, txNow, now);

    s->lastRx = rxNow;
    s->lastTx = txNow;
    s->lastTicks = now;
    s->valid = TRUE;
    return r | NSP_TICK_RATE;
}
