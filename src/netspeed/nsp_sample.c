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

static unsigned long long nsp_now(void)
{
    struct EClockVal ev;
    ReadEClock(&ev);
    return ((unsigned long long)ev.ev_hi << 32) | ev.ev_lo;
}

static unsigned long long nsp_quad(const SBQUAD_T *q)
{
    return ((unsigned long long)q->sbq_High << 32) | q->sbq_Low;
}

/* bytes per second over a span of EClock ticks (> 0), saturated */
static ULONG nsp_rate(unsigned long long bytes, unsigned long long ticks, ULONG freq)
{
    unsigned long long r = bytes * freq / ticks;
    return r > 0xFFFFFFFFULL ? 0xFFFFFFFFUL : (ULONG)r;
}

void nsp_sample_init(struct NspSampler *s, ULONG eclockFreq)
{
    s->ifaces.count = 0;
    s->eclockFreq = eclockFreq;
    s->rescanEvery = 1;
    s->rescanIn = 0;
    s->haveLink = FALSE;
    s->bps = 0;
    s->state = 0;
    nsp_sample_reset(s, TRUE);
}

void nsp_sample_set_interval(struct NspSampler *s, LONG secs)
{
    s->rescanEvery = (NSP_RESCAN_SECS + secs - 1) / secs;
    if (s->rescanIn > s->rescanEvery)
        s->rescanIn = s->rescanEvery;
}

BOOL nsp_sample_rescan(struct NspSampler *s)
{
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

LONG nsp_sample_find(const struct NspSampler *s, const char *name)
{
    if (name[0] == '\0')
        return -1;
    for (ULONG i = 0; i < s->ifaces.count; i++)
        if (strcmp(s->ifaces.names[i], name) == 0)
            return (LONG)i;
    return -1;
}

void nsp_sample_reset(struct NspSampler *s, BOOL dropCounters)
{
    s->valid = FALSE;
    s->curRx = s->curTx = s->avgRx = s->avgTx = s->maxRx = s->maxTx = 0;
    if (dropCounters)
        s->haveLast = FALSE;
    else
    {
        s->baseRx = s->lastRx;
        s->baseTx = s->lastTx;
        s->baseTicks = s->lastTicks;
    }
}

static void nsp_seed(struct NspSampler *s, unsigned long long rx, unsigned long long tx,
                     unsigned long long now)
{
    s->lastRx = s->baseRx = rx;
    s->lastTx = s->baseTx = tx;
    s->lastTicks = s->baseTicks = now;
    s->haveLast = TRUE;
    s->valid = FALSE;
    s->curRx = s->curTx = s->avgRx = s->avgTx = s->maxRx = s->maxTx = 0;
}

LONG nsp_sample_tick(struct NspSampler *s, const char *name)
{
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
    else
    {
        if (QueryInterfaceTags((STRPTR)name, IFQ_GetBytesIn, (Tag)&rx, IFQ_GetBytesOut, (Tag)&tx,
                               IFQ_BPS, (Tag)&bps, IFQ_State, (Tag)&state, TAG_END) != 0)
            return NSP_TICK_FAILED;
        haveLink = TRUE;
    }
    s->haveLink = haveLink;
    s->bps = bps;
    s->state = state;

    unsigned long long now = nsp_now();
    unsigned long long rxNow = nsp_quad(&rx);
    unsigned long long txNow = nsp_quad(&tx);
    unsigned long long ticks = now - s->lastTicks;

    /* first sample, restarted counters (interface re-added) or no time
     * passed: nothing to divide yet */
    if (!s->haveLast || rxNow < s->lastRx || txNow < s->lastTx || ticks == 0)
    {
        nsp_seed(s, rxNow, txNow, now);
        return NSP_TICK_SEED;
    }

    s->curRx = nsp_rate(rxNow - s->lastRx, ticks, s->eclockFreq);
    s->curTx = nsp_rate(txNow - s->lastTx, ticks, s->eclockFreq);
    if (s->curRx > s->maxRx)
        s->maxRx = s->curRx;
    if (s->curTx > s->maxTx)
        s->maxTx = s->curTx;
    unsigned long long sinceBase = now - s->baseTicks; /* >= ticks > 0 */
    s->avgRx = nsp_rate(rxNow - s->baseRx, sinceBase, s->eclockFreq);
    s->avgTx = nsp_rate(txNow - s->baseTx, sinceBase, s->eclockFreq);
    s->lastRx = rxNow;
    s->lastTx = txNow;
    s->lastTicks = now;
    s->valid = TRUE;
    return NSP_TICK_RATE;
}
