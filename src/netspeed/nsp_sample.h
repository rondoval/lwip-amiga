/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * NetSpeed sampler: the interface set, the byte counters of the selected
 * source and the rates derived from them.
 *
 * Source = an interface name (QueryInterfaceTags on it) or "" for all
 * interfaces (the stack-wide SBTC_GET_BYTES_* counters). The stack refreshes
 * both once a second, which is why the sample interval starts at 1 s. Rates
 * are computed against the EClock time actually elapsed, not the interval.
 */

#ifndef NSP_SAMPLE_H
#define NSP_SAMPLE_H

#include "netspeed.h"

struct NspIfaceSet
{
    ULONG count;
    char names[NSP_IFACE_MAX][NSP_NAME_MAX]; /* stack order */
};

struct NspSampler
{
    struct NspIfaceSet ifaces; /* last ObtainInterfaceList() */
    LONG rescanEvery;          /* ticks between interface list polls */
    LONG rescanIn;             /* ticks until the next poll; <= 0 forces one */
    ULONG eclockFreq;

    BOOL haveLast; /* the last* fields hold a sample */
    unsigned long long lastRx, lastTx, lastTicks;
    unsigned long long baseRx, baseTx, baseTicks; /* since Reset: the average */

    BOOL valid; /* cur/avg/max hold at least one rate since Reset */
    ULONG curRx, curTx, avgRx, avgTx, maxRx, maxTx; /* bytes/s */

    BOOL haveLink; /* bps/state were answered this tick */
    LONG bps;      /* IFQ_BPS, bits/s */
    LONG state;    /* IFQ_State: SM_Up / SM_Down */
};

enum
{
    NSP_TICK_FAILED = -1, /* the selected interface is unknown to the stack */
    NSP_TICK_SEED = 0,    /* sample stored, no rate yet */
    NSP_TICK_RATE = 1     /* cur/avg/max updated */
};

void nsp_sample_init(struct NspSampler *s, ULONG eclockFreq);
/* Poll the stack's interface list; TRUE when the set of names changed. */
BOOL nsp_sample_rescan(struct NspSampler *s);
/* Index in the set, -1 if absent ("" is never in the set). */
LONG nsp_sample_find(const struct NspSampler *s, const char *name);
/* Zero the statistics. dropCounters forgets the last sample too (source
 * changed: the next tick seeds); otherwise the next tick yields a rate. */
void nsp_sample_reset(struct NspSampler *s, BOOL dropCounters);
LONG nsp_sample_tick(struct NspSampler *s, const char *name);
void nsp_sample_set_interval(struct NspSampler *s, LONG secs);

#endif
