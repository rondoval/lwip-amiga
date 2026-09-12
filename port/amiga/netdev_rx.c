/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * netdev RX path: driver batches injected into lwIP as zero-copy custom
 * pbufs, with pre-lock checksum verification (RAW-offload fold) and the
 * shared GRO-lite merge of in-order TCP runs (rx_gro.c). Everything here
 * runs on the driver's unit task; lwIP entry is bracketed by the netstack
 * core lock.
 */

#include "netstack_sys.h"

#include <debug.h>

#include <lwip/opt.h> /* before the prot headers: they default LWIP_PLATFORM_
                         macros through lwip/arch.h otherwise */
#include <lwip/prot/ip.h>
#include <lwip/prot/udp.h>

#include "netdev_priv.h"
#include "netstack.h"
#include "nsprof.h"
#include "rx_gro.h"

static void ndif_rx_pbuf_freed(struct pbuf *p)
{
    struct NdRxWrap *w = (struct NdRxWrap *)p;
    struct NetdevIf *ndi = w->nrw_If;

    /* A wrap outliving its interface: this pbuf sat in a socket's receive
     * queue across a (forced) RemoveNetInterface. netdevif_destroy marked
     * the pool dead and leaked it, and the driver reclaimed the buffer at
     * (forced) detach — nothing to release, and the NetdevIf may already
     * describe a successor interface this wrap must not touch. */
    if (ndi == NULL)
        return;

    /* Double-free tripwire: releasing the same wrap twice would push its
     * driver buffer into the recycle ring twice. Debug-tier invariant check,
     * not a production safety net. */
#ifdef DEBUG
    KASSERT(w->nrw_Live == 1, "RX-WRAP-GUARD: double free");
    w->nrw_Live = 0;
#endif

    ndi->ndi_Ops->ndo_RxRelease(ndi->ndi_Drv, w->nrw_Cookie);

    ndi->ndi_WrapsOut--;
    w->nrw_Next = ndi->ndi_FreeWraps;
    ndi->ndi_FreeWraps = w;
}

/* RX verification for frames the driver reported only a RAW checksum for.
 * `raw` is the 1's-complement sum over the frame past the Ethernet
 * header (IP header + payload). A valid IP header folds to -0, so for a
 * valid TCP/UDP checksum fold(raw + pseudo-header) == 0xFFFF. Non-IP,
 * non-TCP/UDP and checksum-less UDP pass through; fragments pass and are
 * validated only by the reassembled IP checksum (documented gap — the
 * Ethernet FCS already covered the wire). */
static BOOL ndif_rx_csum_ok(const struct NetDevRxDesc *d, ULONG raw)
{
    const UBYTE *frame = d->nrd_Data;
    ULONG l3 = inetfrm_ip_offset(frame, d->nrd_Len);
    if (l3 == 0)
        return TRUE; /* non-IPv4: nothing to fold, accept */

    /* `raw` is the 1's-complement sum over [frame + SIZEOF_ETH_HDR .. end].
     * On a tagged frame that region opens with the two 16-bit tag words (the
     * TCI and the inner ethertype) that sit before the IP header, so remove
     * them here — inetfrm_pseudo_sum expects a sum over [IP .. end]. */
    if (l3 != SIZEOF_ETH_HDR)
    {
        const UWORD *tag = (const UWORD *)(frame + SIZEOF_ETH_HDR);
        raw += (~(ULONG)tag[0]) & 0xFFFF;   /* subtract the 802.1Q TCI word */
        raw += (~(ULONG)tag[1]) & 0xFFFF;   /* subtract the inner ethertype word */
    }

    const struct ip_hdr *ip = (const struct ip_hdr *)(frame + l3);
    if ((IPH_OFFSET(ip) & PP_HTONS(IP_OFFMASK | IP_MF)) != 0)
        return TRUE;

    ULONG proto = IPH_PROTO(ip);
    if (proto != IP_PROTO_TCP && proto != IP_PROTO_UDP)
        return TRUE;

    if (proto == IP_PROTO_UDP)
    {
        const struct udp_hdr *uh =
            (const struct udp_hdr *)(frame + l3 + (ULONG)IPH_HL(ip) * 4);
        if (uh->chksum == 0)
            return TRUE; /* UDP without checksum is legal on IPv4 */
    }

    return inetfrm_pseudo_sum(ip, raw) == 0xFFFF;
}

/* ----------------------------------------------------------- injection --- */

ULONG ndif_rx_input(APTR stackctx, const struct NetDevRxDesc *descs, ULONG count)
{
    struct NetdevIf *ndi = stackctx;
    ULONG consumed = 0;
    ULONG since_yield = 0;
    /* GRO rides on the RX csum offload: with lwIP's own TCP checksum check
     * active, a merged (rewritten) header would fail re-verification */
    BOOL gro = ndi->ndi_RxOffload;

    while (consumed < count)
    {
        const struct NetDevRxDesc *cd = &descs[consumed];
        ULONG chunk = count - consumed;
        if (chunk > NDIF_RX_CHUNK)
            chunk = NDIF_RX_CHUNK;

        /* Checksum verdicts + GRO classification BEFORE taking the core
         * lock — both read only frame bytes, and nso_RxInput runs on the
         * driver's unit task alone, so nothing here needs protection.
         * lwIP's TCP/UDP checking is off when the driver offloads RX csum;
         * frames without the VALID verdict get their RAW sum folded here. */
        UBYTE drop[NDIF_RX_CHUNK];
        PERF_T0(t_csum);
        for (ULONG i = 0; i < chunk; i++)
        {
            const struct NetDevRxDesc *d = &cd[i];
            drop[i] = ndi->ndi_RxOffload &&
                      !(d->nrd_Flags & NDRF_CSUM_VALID) &&
                      (d->nrd_Flags & NDRF_CSUM_RAW) &&
                      !ndif_rx_csum_ok(d, d->nrd_CsumRaw);
            if (gro && !drop[i])
                rxgro_classify(d->nrd_Data, d->nrd_Len, &ndi->ndi_GroMeta[i]);
        }
        PERF_ADD(&ns_perf, NSP_RX_CSUM, t_csum);

        PERF_T0(t_lock);
        netstack_lock();
        PERF_ADD(&ns_perf, NSP_RX_LOCKWAIT, t_lock);
        for (ULONG i = 0; i < chunk; i++)
        {
            const struct NetDevRxDesc *d = &cd[i];

            /* a bad frame is consumed and its buffer released immediately
             * (releases stay under the lock: it is the recycle ring's
             * single-producer guarantee). Its headers are untrustworthy,
             * so any held merge run is flushed — a mid-run drop is a
             * sequence discontinuity. */
            if (drop[i])
            {
                rxgro_flush_all(&ndi->ndi_Gro);
                ndi->ndi_RxCsumBad++;
                ndi->ndi_Ops->ndo_RxRelease(ndi->ndi_Drv, d->nrd_Cookie);
                consumed++;
                continue;
            }

            struct NdRxWrap *w = ndi->ndi_FreeWraps;
            if (w == NULL)
            {
                ndi->ndi_RxNoWrap++;
                rxgro_flush_all(&ndi->ndi_Gro); /* held frames are consumed: deliver */
                netstack_unlock();
                return consumed; /* backpressure: driver recycles the tail */
            }
            ndi->ndi_FreeWraps = w->nrw_Next;
            ndi->ndi_WrapsOut++;

            w->nrw_Cookie = d->nrd_Cookie;
            w->nrw_Pc.custom_free_function = ndif_rx_pbuf_freed;
#ifdef DEBUG
            w->nrw_Live = 1;
#endif

            struct pbuf *p = pbuf_alloced_custom(PBUF_RAW, (u16_t)d->nrd_Len,
                                                 PBUF_REF, &w->nrw_Pc,
                                                 d->nrd_Data, (u16_t)d->nrd_Len);
            consumed++;

            rxgro_input(&ndi->ndi_Gro, p, &ndi->ndi_GroMeta[i], gro);

            /* Fairness yield (see RXGRO_YIELD_STRIDE): under contention, hand
             * the FIFO lock to a queued app task so it drains its socket queue
             * mid-batch instead of starving. The loop head re-reads
             * ndi_FreeWraps after the relock, so wraps freed meanwhile are
             * seen. */
            if (++since_yield >= RXGRO_YIELD_STRIDE &&
                netstack.ns_Core.ss_QueueCount > 0)
            {
                since_yield = 0;
                rxgro_yield(&ndi->ndi_Gro);
            }
        }
        rxgro_flush_all(&ndi->ndi_Gro);
        netstack_unlock();
    }

    return consumed;
}
