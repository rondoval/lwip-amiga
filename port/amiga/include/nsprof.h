/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * nsprof — the netstack's instance of the emu68-common perf framework
 * (<perf.h>: per-stage µs timing slots, ~2 s delta reporting under the
 * "[nsprof]" tag, reduced with emu68-common/scripts/perf-report.py).
 *
 * This header carries the slot enum and the `ns_perf` instance the probe
 * sites pass to PERF_ADD; the storage and report cadence live in
 * netstack.c. Probes compile to nothing without DEBUG; the slot storage is
 * unconditional so all build tiers share one layout.
 */

#ifndef LWIPAMIGA_NSPROF_H
#define LWIPAMIGA_NSPROF_H

#include <perf.h>

enum NsProfSlot
{
    /* unit task, RX injection (ndif_rx_input) */
    NSP_RX_CSUM,       /* pre-lock csum fold + GRO classification, per chunk */
    NSP_RX_LOCKWAIT,   /* waiting for ns_Core (initial + yield relock) */
    NSP_RX_INPUT,      /* netif.input() per DELIVERY (a GRO merge counts once;
                          merge ratio = driver frames/s over this slot's n) */
    NSP_RX_GRO,        /* under-lock GRO absorb bookkeeping, per frame */
    /* core-lock holder, deferred TX reclaim drain (netdevif_tx_reclaim) */
    NSP_TX_DONE,       /* batched pbuf_free of completed cookies, folded into an
                          existing hold (no lock wait; ndif_tx_done only enqueues) */
    /* any task, under the lock (ndif_linkoutput, from tcp_output/udp_send) */
    NSP_TX_LINKOUT,    /* whole linkoutput per frame: offsets+seed+submit */
    NSP_TX_SUBMIT,     /* driver ndo_TxSubmit within it: ring+cache+doorbell */
    /* app task, sb_tcp_recv */
    NSP_RECV_LOCKWAIT, /* waiting for ns_Core (entry + post-copy relock) */
    NSP_RECV_COPY,     /* copy-out of a detached run, lock released */
    NSP_RECV_ACKFLUSH, /* tcp_recved window-update flush per drain pass */
    NSP_RECV_SLEEP,    /* blocked waiting for data */
    /* app task, sb_tcp_send / sb_dgram_send */
    NSP_SEND_LOCKWAIT, /* waiting for ns_Core (entry + chunk-break relock) */
    NSP_SEND_WRITE,    /* tcp_write per chunk: pbuf alloc + memcpy */
    NSP_SEND_OUTPUT,   /* tcp_output per call: segmentation -> linkoutput */
    NSP_SEND_SLEEP,    /* blocked on a full send buffer */
    NSP_UDP_SEND,      /* dgram send under the lock: alloc+copy+udp/raw out */

    NSP_SLOT_COUNT
};

/* the netstack instance (netstack.c); reported from netstack_tick */
extern struct perf ns_perf;

#endif /* LWIPAMIGA_NSPROF_H */
