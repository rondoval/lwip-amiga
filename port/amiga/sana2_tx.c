/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * SANA-II TX path: lwIP's built Ethernet frames staged as cooked write
 * requests under the core lock, submitted quick (IOF_QUICK BeginIO) in one
 * batch at the outermost netstack_unlock (the netdev tx-kick idiom).
 * Synchronous drivers complete in place — no ReplyMsg, no pump wakeup;
 * queuing drivers clear IOF_QUICK and those writes complete on the pump.
 */

#include "netstack_sys.h"

#include <debug.h>

#include <lwip/opt.h> /* before the prot headers: they default LWIP_PLATFORM_
                         macros through lwip/arch.h otherwise */
#include <lwip/prot/ethernet.h>

#include "netstack.h"
#include "nsprof.h"
#include "sana2_priv.h"

/* netif->linkoutput — under the core lock, in whatever task called lwIP.
 * The frame arrives with its Ethernet header built (etharp/ethernet_output,
 * always contiguous in the first pbuf); cooked mode moves the header into
 * the request fields and ships the body from offset 14. Under in-band VLAN
 * the outer ethertype 0x8100 lands in ios2_PacketType and the body starts
 * at the TCI — the same uniform 14-byte skip. */
err_t s2if_linkoutput(struct netif *nif, struct pbuf *p)
{
    struct Sana2If *s2i = nif->state;
    PERF_T0(t_out);

    if (s2i->s2i_TxDown)
        return ERR_IF; /* teardown: the pump (the reply port) is going away */

    if (p->len < SIZEOF_ETH_HDR)
    {
        /* unreachable off ethernet_output (the header is prepended into the
         * first pbuf); refuse rather than parse a split header */
        s2i->s2i_TxDrops++;
        return ERR_IF;
    }

    struct S2TxReq *req = s2i->s2i_TxFree;
    if (req == NULL)
    {
        s2i->s2i_TxDrops++;
        return ERR_MEM; /* backpressure; TCP retries on timer */
    }

    /* Aliasing guard (the ndif_linkoutput idiom): a ref beyond the owner's
     * single hold means an earlier transmission of this very pbuf is still
     * in the driver — deferred-copy drivers CopyFromBuff on their own task —
     * and lwIP rewrites queued TCP headers in place on retransmit. Ship a
     * private copy instead of a torn frame. */
    struct pbuf *frame = p;
    if (p->ref > 1)
    {
        frame = pbuf_clone(PBUF_RAW, PBUF_RAM, p);
        if (frame == NULL)
            return ERR_MEM; /* retry later; the armed copy stays intact */
    }
    else
        pbuf_ref(frame); /* the driver reads it until the reply */

    s2i->s2i_TxFree = req->stx_Next;

    const UBYTE *eth = frame->payload;
    struct IOSana2Req *io = &req->stx_Io;
    BOOL bcast = (eth[0] & eth[1] & eth[2] & eth[3] & eth[4] & eth[5]) == 0xFF;
    io->ios2_Req.io_Command = bcast          ? S2_BROADCAST
                              : (eth[0] & 1) ? S2_MULTICAST
                                             : CMD_WRITE;
    io->ios2_Req.io_Flags = 0;
    io->ios2_Req.io_Error = 0;
    for (int i = 0; i < 6; i++)
        io->ios2_DstAddr[i] = eth[i];
    io->ios2_PacketType = ((ULONG)eth[12] << 8) | eth[13];
    io->ios2_DataLength = (ULONG)frame->tot_len - SIZEOF_ETH_HDR;
    io->ios2_Data = frame; /* the CopyFromBuff cookie */

    req->stx_Next = NULL;
    *s2i->s2i_TxStagedTail = req;
    s2i->s2i_TxStagedTail = &req->stx_Next;
    s2i->s2i_TxFlushPending = TRUE;

    s2if_u64_add(&s2i->s2i_TxBytesHi, &s2i->s2i_TxBytesLo, frame->tot_len);
    PERF_ADD(&ns_perf, NSP_TX_LINKOUT, t_out);
    return ERR_OK;
}

/* Direct device BeginIO (LVO -30). exec's SendIO veneer unconditionally
 * clears IOF_QUICK, so quick submission must jump the vector itself; the
 * library links freestanding, without amiga.lib's BeginIO stub. */
static void s2if_begin_io(struct IORequest *ior)
{
    register struct IORequest *reqA1 asm("a1") = ior;
    register struct Device *devA6 asm("a6") = ior->io_Device;
    asm volatile("jsr -30(%%a6)"
                 : "+r"(reqA1)
                 : "r"(devA6)
                 : "d0", "d1", "a0", "cc", "memory");
}

/* Retire one finished write: error count, pbuf release, back to the free
 * pool. Shared by the quick path (in place, never in flight) and the pump
 * harvest. Under the core lock either way. */
static void s2if_tx_retire(struct Sana2If *s2i, struct S2TxReq *req)
{
    if (req->stx_Io.ios2_Req.io_Error != 0)
        s2i->s2i_TxErrors++;
    pbuf_free((struct pbuf *)req->stx_Io.ios2_Data);
    req->stx_Io.ios2_Data = NULL;
    req->stx_Next = s2i->s2i_TxFree;
    s2i->s2i_TxFree = req;
}

/* Publish the staged batch: called at every outermost netstack_unlock,
 * still under the lock, NULL-tolerant. BeginIO under the lock is
 * deadlock-free — SANA-II drivers never take ns_Core, and the only client
 * code a synchronous BeginIO calls back (the buffer callbacks) is
 * lock-free; an inline-TX driver costs bounded copy time here, the same
 * order as netdev's under-lock ndo_TxSubmit.
 *
 * Each request goes out with IOF_QUICK: a synchronous driver (genet-sana2)
 * leaves the flag set and the write retires right here — no ReplyMsg from
 * the driver, no pump wakeup, no per-frame task switch. A queuing driver
 * (Poseidon cdceth/lan78xx unit tasks) clears the flag per the quick-I/O
 * contract; only those writes enter TxInFlight and reply to the pump. The
 * pump cannot race the in-flight accounting: it harvests replies under
 * this same lock. */
void sana2if_tx_flush(struct Sana2If *s2i)
{
    if (s2i == NULL || !s2i->s2i_TxFlushPending)
        return;

    struct S2TxReq *req = s2i->s2i_TxStagedHead;
    s2i->s2i_TxStagedHead = NULL;
    s2i->s2i_TxStagedTail = &s2i->s2i_TxStagedHead;
    s2i->s2i_TxFlushPending = FALSE;

    PERF_T0(t_sub);
    while (req != NULL)
    {
        struct S2TxReq *next = req->stx_Next;
        req->stx_Io.ios2_Req.io_Flags = IOF_QUICK;
        s2if_begin_io((struct IORequest *)&req->stx_Io);
        if (req->stx_Io.ios2_Req.io_Flags & IOF_QUICK)
            s2if_tx_retire(s2i, req); /* completed in place */
        else
            s2i->s2i_TxInFlight++; /* queued; replies to the pump */
        req = next;
    }
    PERF_ADD(&ns_perf, NSP_TX_SUBMIT, t_sub);
}

/* One deferred write's reply, harvested by the pump. Under the core lock. */
void s2if_tx_complete(struct Sana2If *s2i, struct S2TxReq *req)
{
    s2if_tx_retire(s2i, req);
    s2i->s2i_TxInFlight--;
}
