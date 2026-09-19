/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * NetSpeed sampler: the interface set, the byte counters of the selected
 * source and the rates derived from them.
 *
 * Source = an interface name (QueryInterfaceTags on it) or "" for all
 * interfaces (the stack-wide SBTC_GET_BYTES_* counters). The stack refreshes
 * both once a second, which is the sample period. Rates are computed against
 * the EClock time actually elapsed, not the nominal period.
 *
 * Current = the last sample. Maximum = highest sample since Reset. Average =
 * bytes over time across a moving window of the last N samples, or since
 * Reset when the window is 0.
 *
 * The sampler polls the stack's interface list by itself, every
 * NSP_RESCAN_SECS and right after a query that failed, and reports a
 * changed set in the tick result.
 */

#ifndef NSP_SAMPLE_H
#define NSP_SAMPLE_H

#include "netspeed.h"

struct NspIfaceSet
{
    ULONG count;
    char names[NSP_IFACE_MAX][NSP_NAME_MAX]; /* stack order */
};

/* one sample: the tick's byte deltas and its duration */
struct NspSample
{
    ULONG rx, tx; /* bytes */
    ULONG ticks;  /* EClock ticks */
};

struct NspSampler
{
    struct NspIfaceSet ifaces; /* last interface list poll */
    LONG rescanIn;             /* ticks until the next poll */
    ULONG eclockFreq;

    BOOL haveLast; /* the last* fields hold a sample */
    unsigned long long lastRx, lastTx, lastTicks;
    unsigned long long baseRx, baseTx, baseTicks; /* since Reset */

    struct NspSample hist[NSP_AVG_MAX]; /* newest at histHead - 1 */
    ULONG histHead, histCount;
    ULONG avgSamples; /* moving window length; 0 = since Reset */

    BOOL valid; /* cur/avg/max hold at least one rate since Reset */
    ULONG curRx, curTx, avgRx, avgTx, maxRx, maxTx; /* bytes/s */

    BOOL haveLink;   /* bps/state were answered this tick */
    LONG bps;        /* IFQ_BPS, bits/s */
    LONG state;      /* IFQ_State: SM_Up / SM_Down */
    ULONG linkBytes; /* bytes/s of an up link, 0 when down or unknown */
};

/* nsp_sample_tick() result bits */
#define NSP_TICK_RATE (1UL << 0)   /* cur/avg/max updated (else only seeded) */
#define NSP_TICK_IFACES (1UL << 1) /* the interface set changed */
#define NSP_TICK_FAILED (1UL << 2) /* the interface is unknown to the stack */

/* Polls the interface list once. */
void nsp_sample_init(struct NspSampler *s, ULONG eclockFreq);
/* Forget the previous source and take the first sample of this one, so the
 * next tick already yields a rate. FALSE: unknown interface. */
BOOL nsp_sample_select(struct NspSampler *s, const char *name);
/* Zero the statistics but keep the last sample: the next tick yields a rate
 * and "since Reset" starts here. */
void nsp_sample_reset(struct NspSampler *s);
/* One sample of the source; returns NSP_TICK_* bits. */
ULONG nsp_sample_tick(struct NspSampler *s, const char *name);
/* Average window in seconds, 0 = since Reset; applies from the next tick. */
void nsp_sample_set_average(struct NspSampler *s, LONG secs);

#endif
