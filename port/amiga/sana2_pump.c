/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The SANA-II RX pump: the client-side analog of a netdev driver's unit
 * task. SANA-II has no upcall — received frames only ever surface as
 * completed CMD_READ requests — so this per-interface task keeps a pool of
 * typed reads posted (IPv4 + ARP, plus 0x8100 under in-band VLAN), harvests
 * replies (reads, the staged writes' completions, and the S2_ONEVENT link
 * tracker all reply to the one pump-owned port), feeds lwIP in batched
 * core-lock holds and reposts.
 *
 * Buffer discipline: every read owns a PBUF_RAM sized for the driver's MTU
 * with 14 bytes of headroom; the driver copies the cooked payload past the
 * headroom (CopyToBuff cookie), the pump synthesizes the Ethernet header in
 * front and hands the pbuf to lwIP, replacing it with a fresh one. When the
 * replacement allocation fails the FRAME is dropped and the pbuf reused —
 * the read depth never decays, so the pump cannot starve itself of wakeups.
 *
 * Lifecycle: started before netif_set_up (the TX pool's reply ports must be
 * stamped before anything can stage a write) with a ready handshake;
 * stopped with CTRL_C — the pump AbortIO()s its reads and the event
 * request, then drains until every read is home AND every in-flight write
 * has completed, frees its pbufs/port and signals back. After stop the
 * driver holds no pointer of ours.
 */

#include "netstack_sys.h"

#include <dos/dosextens.h>
#include <dos/dostags.h>

#ifdef __INTELLISENSE__
#include <clib/dos_protos.h>
#else
#include <proto/dos.h>
#endif

#include <debug.h>
#include <memory.h>

#include <lwip/netif.h>
#include <lwip/inet_chksum.h>
#include <lwip/prot/ethernet.h>
#include <lwip/prot/ip.h>
#include <lwip/prot/tcp.h>

#include "inet_frame.h"
#include "netstack.h"
#include "nsprof.h"
#include "rx_gro.h"
#include "sana2_priv.h"

/* How the freshly created pump process finds its interface: the library is
 * a singleton with one active interface, and bsdsocket.library is
 * deliberately not ROM-able (writable statics allowed), so a file-scope
 * handoff slot beats inventing a startup-message protocol.
 * Written by pump_start before CreateNewProcTags, read once by
 * the new process. */
static struct Sana2If *s2if_pump_new;

/* Pump-task state, stack-resident in s2if_pump_task, one pointer passed to
 * every helper. */
struct S2Pump
{
    struct Sana2If *s2p_If;
    struct netif *s2p_Netif;
    struct S2RxReq *s2p_Reads; /* AllocMem'd request array */
    ULONG s2p_NumReads;
    ULONG s2p_StorSize;
    ULONG s2p_PbLen;  /* per-read pbuf size */
    BOOL s2p_Gro;     /* GRO-lite gate, see s2if_pump_setup */
    struct MsgPort *s2p_Port;
    ULONG s2p_PortSig;
    struct IOSana2Req s2p_Ev; /* the S2_ONEVENT link tracker */
    BOOL s2p_EvHome, s2p_EvDead;
    ULONG s2p_Flight;
    struct S2RxReq *s2p_Parked;
    BOOL s2p_Draining;
    /* per-wake harvest batches: s2if_pump_harvest fills, txev/rx consume */
    struct S2TxReq *s2p_TxDone;
    struct S2RxReq *s2p_RxDone;
    BOOL s2p_EvReplied;
};

static void s2if_rx_arm(struct S2RxReq *r, struct pbuf *pb)
{
    r->srx_Pbuf = pb;
    r->srx_Dst = (UBYTE *)pb->payload + SIZEOF_ETH_HDR;
    r->srx_Cap = (ULONG)pb->len - SIZEOF_ETH_HDR;
}

/* Detach the filled pbuf from a completed read: synthesize the Ethernet
 * header the cooked read stripped, trim to the received length, count the
 * bytes. Under the core lock (pbuf_realloc may touch the heap). The frame
 * must be taken BEFORE the request is reposted — the driver reuses the
 * ios2 fields the moment it gets the request back. */
static struct pbuf *s2if_rx_detach(struct Sana2If *s2i, struct S2RxReq *r)
{
    struct IOSana2Req *io = &r->srx_Io;
    struct pbuf *p = r->srx_Pbuf;
    UBYTE *hdr = p->payload;

    for (int i = 0; i < 6; i++)
    {
        hdr[i] = io->ios2_DstAddr[i];
        hdr[6 + i] = io->ios2_SrcAddr[i];
    }
    hdr[12] = (UBYTE)(io->ios2_PacketType >> 8);
    hdr[13] = (UBYTE)io->ios2_PacketType;

    pbuf_realloc(p, (u16_t)(SIZEOF_ETH_HDR + io->ios2_DataLength));
    s2if_u64_add(&s2i->s2i_RxBytesHi, &s2i->s2i_RxBytesLo, p->tot_len);

    r->srx_Pbuf = NULL;
    return p;
}

/* Pre-lock TCP checksum verdict for one detached frame (contiguous
 * PBUF_RAM, pump-private off the lock). lwIP's own TCP check is off for
 * this netif (s2if_netif_init): a GRO-merged header must not be
 * re-verified, so every delivered TCP frame is verified here instead.
 * Non-IPv4, non-TCP and fragments pass through (fragments are validated
 * only by the reassembled IP checksum — the documented netdev-parity gap;
 * UDP and IP-header checking stay with lwIP). The length bound is
 * mandatory, not polish: the sum trusts IPH_LEN, and a lying header must
 * not read past the pbuf — under-bound frames pass through here and die
 * on lwIP's own length checks instead. */
static BOOL s2if_rx_csum_ok(const struct pbuf *p)
{
    const UBYTE *frame = p->payload;
    ULONG l3 = inetfrm_ip_offset(frame, p->len);
    if (l3 == 0)
        return TRUE; /* non-IPv4 */

    const struct ip_hdr *ip = (const struct ip_hdr *)(frame + l3);
    if (IPH_PROTO(ip) != IP_PROTO_TCP)
        return TRUE;
    if ((IPH_OFFSET(ip) & PP_HTONS(IP_OFFMASK | IP_MF)) != 0)
        return TRUE; /* fragment */

    ULONG ihl = (ULONG)IPH_HL(ip) * 4;
    ULONG iplen = lwip_ntohs(IPH_LEN(ip));
    if (iplen < ihl + TCP_HLEN || l3 + iplen > p->len)
        return TRUE; /* bounds lie: lwIP's length checks reject it */

    UWORD sum = (UWORD)~inet_chksum(frame + l3 + ihl, (u16_t)(iplen - ihl));
    return inetfrm_pseudo_sum(ip, sum) == 0xFFFF;
}

/* The fields every pump-owned IOSana2Req shares (reads and the event
 * tracker; the TX pool's requests are initialized at create and only get
 * their reply port stamped here). */
static void s2if_pump_io_init(struct S2Pump *pp, struct IOSana2Req *io,
                              UWORD cmd)
{
    io->ios2_Req.io_Message.mn_ReplyPort = pp->s2p_Port;
    io->ios2_Req.io_Message.mn_Length = sizeof(struct IOSana2Req);
    io->ios2_Req.io_Device = pp->s2p_If->s2i_Device;
    io->ios2_Req.io_Unit = pp->s2p_If->s2i_Unit;
    io->ios2_Req.io_Command = cmd;
    io->ios2_BufferManagement = pp->s2p_If->s2i_BufMgmt;
}

/* One pbuf per read, armed in bounded lock holds (the pool covers the
 * whole TCP window — hundreds of buffers). On allocation failure every
 * pbuf armed so far is freed under the same failing hold. */
static BOOL s2if_pump_arm_pbufs(struct S2Pump *pp)
{
    ULONG armed = 0;
    while (armed < pp->s2p_NumReads)
    {
        ULONG stop = armed + S2IF_RX_BATCH;
        if (stop > pp->s2p_NumReads)
            stop = pp->s2p_NumReads;
        netstack_lock();
        for (; armed < stop; armed++)
        {
            struct pbuf *pb =
                pbuf_alloc(PBUF_RAW, (u16_t)pp->s2p_PbLen, PBUF_RAM);
            if (pb == NULL)
                break;
            s2if_rx_arm(&pp->s2p_Reads[armed], pb);
        }
        if (armed < stop)
        {
            for (ULONG i = 0; i < armed; i++)
                pbuf_free(pp->s2p_Reads[i].srx_Pbuf);
            netstack_unlock();
            return FALSE;
        }
        netstack_unlock();
    }
    return TRUE;
}

/* Everything up to (but not including) the ready handshake: sizing, port
 * and request-array allocation, request init, TX-pool reply-port stamping,
 * pbuf arming, the initial read burst and the link-tracker arm. Returns
 * 0/-1; the caller signals either way and frees on failure. */
static LONG s2if_pump_setup(struct S2Pump *pp, struct Sana2If *s2i)
{
    memset(pp, 0, sizeof(*pp));
    pp->s2p_If = s2i;
    struct NetIfBase *nib = &s2i->s2i_Base;
    pp->s2p_Netif = &nib->nib_Netif;

    LONG vlan = nib->nib_VlanTci;
    ULONG nIp4, nArp, nVlan;
    s2if_rx_classes(vlan, &nIp4, &nArp, &nVlan);
    pp->s2p_NumReads = nIp4 + nArp + nVlan;
    /* the driver may deliver up to ITS mtu regardless of our netif clamp;
     * +4 for the tag words a 0x8100 cooked read carries as payload */
    pp->s2p_PbLen = SIZEOF_ETH_HDR + (ULONG)nib->nib_HwMtu +
                    (vlan >= 0 ? 4 : 0) + S2IF_RX_SLACK;
    /* GRO-lite gate: RXGRO_MAX_FRAMES' u16 IPH_LEN ceiling math assumes
     * 1500-byte frames — a jumbo driver would overflow it. TCP checksums
     * are verified pre-lock either way (lwIP's check is off per netif). */
    pp->s2p_Gro = nib->nib_HwMtu <= 1500;

    pp->s2p_Port = CreateMsgPort();
    pp->s2p_StorSize = pp->s2p_NumReads * sizeof(struct S2RxReq);
    pp->s2p_Reads = AllocMem(pp->s2p_StorSize, MEMF_PUBLIC | MEMF_CLEAR);
    if (pp->s2p_Port == NULL || pp->s2p_Reads == NULL)
        return -1;
    pp->s2p_PortSig = 1UL << pp->s2p_Port->mp_SigBit;

    for (ULONG i = 0; i < pp->s2p_NumReads; i++)
    {
        struct IOSana2Req *io = &pp->s2p_Reads[i].srx_Io;
        s2if_pump_io_init(pp, io, CMD_READ);
        io->ios2_PacketType = i < nIp4          ? ETHTYPE_IP
                              : i < nIp4 + nArp ? ETHTYPE_ARP
                                                : ETHTYPE_VLAN;
        io->ios2_Data = &pp->s2p_Reads[i];
    }
    s2if_pump_io_init(pp, &pp->s2p_Ev, S2_ONEVENT);

    /* every write in the pool replies here too; the whole pool sits on the
     * free list until the first linkoutput, which cannot run before the
     * ready signal (the netif is still down) */
    for (struct S2TxReq *t = s2i->s2i_TxFree; t != NULL; t = t->stx_Next)
        t->stx_Io.ios2_Req.io_Message.mn_ReplyPort = pp->s2p_Port;

    if (!s2if_pump_arm_pbufs(pp))
        return -1;

    for (ULONG i = 0; i < pp->s2p_NumReads; i++)
    {
        pp->s2p_Reads[i].srx_State = S2RX_FLIGHT;
        SendIO(&pp->s2p_Reads[i].srx_Io.ios2_Req);
        pp->s2p_Flight++;
    }

    /* arm the link tracker for both edges: an immediate preset reply (the
     * device is online — bring-up just onlined it) seeds the re-arm cycle */
    pp->s2p_Ev.ios2_WireError = S2EVENT_ONLINE | S2EVENT_OFFLINE;
    SendIO(&pp->s2p_Ev.ios2_Req);
    return 0;
}

/* NULL-tolerant: serves both the setup-failure path and final teardown. */
static void s2if_pump_free(struct S2Pump *pp)
{
    if (pp->s2p_Reads != NULL)
        FreeMem(pp->s2p_Reads, pp->s2p_StorSize);
    if (pp->s2p_Port != NULL)
        DeleteMsgPort(pp->s2p_Port);
}

static void s2if_pump_abort(struct S2Pump *pp)
{
    pp->s2p_Draining = TRUE;
    for (ULONG i = 0; i < pp->s2p_NumReads; i++)
    {
        if (pp->s2p_Reads[i].srx_State == S2RX_FLIGHT)
            AbortIO(&pp->s2p_Reads[i].srx_Io.ios2_Req);
    }
    if (!pp->s2p_EvHome && !pp->s2p_EvDead)
        AbortIO(&pp->s2p_Ev.ios2_Req);
}

/* Harvest the port into the per-wake batches. Reads are chained FIFO (the
 * tail-append idiom): delivery order == reply order == wire order — TCP
 * depends on it. Write completions are order-free. */
static void s2if_pump_harvest(struct S2Pump *pp)
{
    pp->s2p_TxDone = NULL;
    pp->s2p_RxDone = NULL;
    pp->s2p_EvReplied = FALSE;
    struct S2RxReq **rxTail = &pp->s2p_RxDone;
    struct Message *m;
    while ((m = GetMsg(pp->s2p_Port)) != NULL)
    {
        struct IOSana2Req *io = (struct IOSana2Req *)m;
        if (io == &pp->s2p_Ev)
        {
            pp->s2p_EvReplied = TRUE;
        }
        else if (io->ios2_Req.io_Command == CMD_READ)
        {
            struct S2RxReq *r = (struct S2RxReq *)io;
            r->srx_Next = NULL;
            *rxTail = r;
            rxTail = &r->srx_Next;
        }
        else /* CMD_WRITE / S2_BROADCAST / S2_MULTICAST */
        {
            struct S2TxReq *t = (struct S2TxReq *)io;
            t->stx_Next = pp->s2p_TxDone;
            pp->s2p_TxDone = t;
        }
    }
}

/* TX completions + the link event: one short hold, then the off-lock
 * follow-ups (event re-arm, unparked-read reposts). */
static void s2if_pump_txev(struct S2Pump *pp)
{
    struct Sana2If *s2i = pp->s2p_If;
    struct netif *nf = pp->s2p_Netif;
    struct S2RxReq *unparked = NULL;
    BOOL rearm = FALSE;
    ULONG rearmMask = 0;
    netstack_lock();
    while (pp->s2p_TxDone != NULL)
    {
        struct S2TxReq *t = pp->s2p_TxDone;
        pp->s2p_TxDone = t->stx_Next;
        s2if_tx_complete(s2i, t);
    }
    if (pp->s2p_EvReplied)
    {
        pp->s2p_EvHome = TRUE;
        if (!pp->s2p_Draining)
        {
            BYTE err = pp->s2p_Ev.ios2_Req.io_Error;
            if (err == 0)
            {
                ULONG fired = pp->s2p_Ev.ios2_WireError;
                if (fired & S2EVENT_OFFLINE)
                    netif_set_link_down(nf);
                if (fired & S2EVENT_ONLINE)
                {
                    netif_set_link_up(nf);
                    unparked = pp->s2p_Parked; /* back in service, off the lock */
                    pp->s2p_Parked = NULL;
                }
                rearm = TRUE;
                rearmMask = netif_is_link_up(nf) ? S2EVENT_OFFLINE
                                                 : S2EVENT_ONLINE;
            }
            else
            {
                /* no usable link events from this driver: the link was
                 * seeded up at bring-up and stays there */
                Kprintf("[sana2if] S2_ONEVENT unsupported (%ld) — link "
                        "tracking off\n", (LONG)err);
                pp->s2p_EvDead = TRUE;
            }
        }
    }
    netstack_unlock();

    if (rearm && !pp->s2p_Draining)
    {
        pp->s2p_Ev.ios2_WireError = rearmMask;
        pp->s2p_EvHome = FALSE;
        SendIO(&pp->s2p_Ev.ios2_Req);
    }
    while (unparked != NULL)
    {
        struct S2RxReq *r = unparked;
        unparked = r->srx_Next;
        r->srx_State = S2RX_FLIGHT;
        SendIO(&r->srx_Io.ios2_Req);
        pp->s2p_Flight++;
    }
}

/* Phase A: consume up to one batch off the harvest chain in one hold —
 * detach each frame into @deliver, arm a replacement and build the repost
 * chain — then SendIO the reposts off the lock. Returns the deliver
 * count. */
static ULONG s2if_pump_rx_requeue(struct S2Pump *pp, struct pbuf **deliver)
{
    struct Sana2If *s2i = pp->s2p_If;
    ULONG nDeliver = 0;
    struct S2RxReq *repost = NULL, **repostTail = &repost;
    ULONG n = 0;

    PERF_T0(t_lock);
    netstack_lock();
    PERF_ADD(&ns_perf, NSP_RX_LOCKWAIT, t_lock);
    PERF_T0(t_req);
    while (pp->s2p_RxDone != NULL && n < S2IF_RX_BATCH)
    {
        struct S2RxReq *r = pp->s2p_RxDone;
        pp->s2p_RxDone = r->srx_Next;
        n++;
        pp->s2p_Flight--;

        BYTE err = r->srx_Io.ios2_Req.io_Error;
        if (pp->s2p_Draining)
        {
            r->srx_State = S2RX_HOME; /* content no longer wanted */
            continue;
        }
        if (err == S2ERR_OUTOFSERVICE)
        {
            /* offline: parking beats a repost spin at 100% CPU;
             * the ONLINE event releases the list */
            r->srx_State = S2RX_PARKED;
            r->srx_Next = pp->s2p_Parked;
            pp->s2p_Parked = r;
            continue;
        }
        if (err == 0 && r->srx_Io.ios2_DataLength <= r->srx_Cap)
        {
            struct pbuf *rep =
                pbuf_alloc(PBUF_RAW, (u16_t)pp->s2p_PbLen, PBUF_RAM);
            if (rep == NULL)
            {
                s2i->s2i_RxNoMem++; /* drop the frame, keep the read */
            }
            else
            {
                deliver[nDeliver++] = s2if_rx_detach(s2i, r);
                s2if_rx_arm(r, rep);
            }
        }
        else
        {
            s2i->s2i_RxErrors++; /* driver-side abort/error: repost
                                    keeps the depth; a lying
                                    ios2_DataLength lands here too */
        }
        r->srx_State = S2RX_FLIGHT;
        r->srx_Next = NULL;
        *repostTail = r;
        repostTail = &r->srx_Next;
        pp->s2p_Flight++;
    }
    PERF_ADD(&ns_perf, NSP_S2_REQUEUE, t_req);
    netstack_unlock();

    while (repost != NULL)
    {
        struct S2RxReq *r = repost;
        repost = r->srx_Next;
        SendIO(&r->srx_Io.ios2_Req);
    }
    return nDeliver;
}

/* Phase B: feed one batch of detached frames to lwIP. Off-lock first: TCP
 * checksum verdicts + GRO classification — both read only frame bytes of
 * pump-private pbufs (the ndif_rx_input pre-lock pass). Then one hold
 * dispatching through the GRO engine with the netdev-style fairness
 * yield — every held merge run is flushed before any lock release. */
static void s2if_pump_rx_deliver(struct S2Pump *pp, struct pbuf **deliver,
                                 ULONG nDeliver)
{
    if (nDeliver == 0)
        return;

    struct Sana2If *s2i = pp->s2p_If;
    BOOL gro = pp->s2p_Gro;
    UBYTE bad[S2IF_RX_BATCH];
    PERF_T0(t_csum);
    for (ULONG i = 0; i < nDeliver; i++)
    {
        struct pbuf *p = deliver[i];
        bad[i] = !s2if_rx_csum_ok(p);
        if (gro && !bad[i])
            rxgro_classify(p->payload, p->len, &s2i->s2i_GroMeta[i]);
    }
    PERF_ADD(&ns_perf, NSP_RX_CSUM, t_csum);

    PERF_T0(t_lock);
    netstack_lock();
    PERF_ADD(&ns_perf, NSP_RX_LOCKWAIT, t_lock);
    ULONG since = 0;
    for (ULONG i = 0; i < nDeliver; i++)
    {
        struct pbuf *p = deliver[i];
        if (bad[i])
        {
            /* untrustworthy headers are a sequence discontinuity:
             * deliver any held run, drop this */
            rxgro_flush_all(&s2i->s2i_Gro);
            s2i->s2i_RxCsumBad++;
            pbuf_free(p);
            continue;
        }
        rxgro_input(&s2i->s2i_Gro, p, &s2i->s2i_GroMeta[i], gro);
        /* fairness yield (see RXGRO_YIELD_STRIDE): under contention, hand
         * the FIFO lock to a queued app task so it drains its socket
         * mid-batch */
        if (++since >= RXGRO_YIELD_STRIDE &&
            netstack.ns_Core.ss_QueueCount > 0)
        {
            since = 0;
            rxgro_yield(&s2i->s2i_Gro);
        }
    }
    rxgro_flush_all(&s2i->s2i_Gro);
    netstack_unlock();
}

/* Read completions, in chunks: requeue FIRST, run lwIP SECOND. The driver
 * drops any frame that finds no pending read, so the reads must go back
 * into service before this task disappears into TCP input for hundreds of
 * microseconds. */
static void s2if_pump_rx(struct S2Pump *pp)
{
    while (pp->s2p_RxDone != NULL)
    {
        struct pbuf *deliver[S2IF_RX_BATCH];
        ULONG nDeliver = s2if_pump_rx_requeue(pp, deliver);
        s2if_pump_rx_deliver(pp, deliver, nDeliver);
    }
}

static BOOL s2if_pump_drained(struct S2Pump *pp)
{
    netstack_lock();
    BOOL done = pp->s2p_Flight == 0 && (pp->s2p_EvHome || pp->s2p_EvDead) &&
                pp->s2p_If->s2i_TxInFlight == 0;
    netstack_unlock();
    return done;
}

static void s2if_pump_task(void)
{
    struct Sana2If *s2i = s2if_pump_new;
    struct S2Pump pump;

    if (s2if_pump_setup(&pump, s2i) != 0)
    {
        s2if_pump_free(&pump);
        s2i->s2i_PumpRc = -1;
        Signal(s2i->s2i_PumpWaiter, SIGBREAKF_CTRL_F);
        return;
    }

    s2i->s2i_PumpPort = pump.s2p_Port;
    s2i->s2i_PumpRc = 0;
    Signal(s2i->s2i_PumpWaiter, SIGBREAKF_CTRL_F);

    for (;;)
    {
        ULONG sigs = Wait(pump.s2p_PortSig | SIGBREAKF_CTRL_C);
        if ((sigs & SIGBREAKF_CTRL_C) && !pump.s2p_Draining)
            s2if_pump_abort(&pump);

        s2if_pump_harvest(&pump);
        s2if_pump_txev(&pump);
        s2if_pump_rx(&pump);

        if (pump.s2p_Draining && s2if_pump_drained(&pump))
            break;
    }

    netstack_lock();
    for (ULONG i = 0; i < pump.s2p_NumReads; i++)
    {
        if (pump.s2p_Reads[i].srx_Pbuf != NULL)
        {
            pbuf_free(pump.s2p_Reads[i].srx_Pbuf);
            pump.s2p_Reads[i].srx_Pbuf = NULL;
        }
    }
    netstack_unlock();

    s2if_pump_free(&pump);
    s2i->s2i_PumpPort = NULL;
    Signal(s2i->s2i_PumpWaiter, SIGBREAKF_CTRL_F);
}

LONG sana2if_pump_start(struct Sana2If *s2i)
{
    Kprintf("[sana2if] %s\n", __func__);
    struct DosLibrary *DOSBase =
        (struct DosLibrary *)OpenLibrary((CONST_STRPTR) "dos.library", 36);
    if (DOSBase == NULL)
        return -1;

    s2i->s2i_PumpRc = -1;
    s2i->s2i_PumpWaiter = FindTask(NULL);
    s2if_pump_new = s2i;
    SetSignal(0UL, SIGBREAKF_CTRL_F);

    /* same tier as the stack task and the netdev unit tasks: above the
     * Executive dynamic-scheduler band, so an app CPU burst cannot starve
     * RX servicing */
    struct Process *proc = CreateNewProcTags(
        NP_Entry, (ULONG)s2if_pump_task,
        NP_Name, (ULONG) "bsdsocket.library sana2 pump",
        NP_Priority, 10,
        NP_StackSize, S2IF_PUMP_STACK,
        TAG_DONE);
    CloseLibrary((struct Library *)DOSBase);
    if (proc == NULL)
        return -1;

    Wait(SIGBREAKF_CTRL_F);
    if (s2i->s2i_PumpRc != 0)
        return -1; /* the pump cleaned up and exited */
    s2i->s2i_Pump = proc;
    return 0;
}

void sana2if_pump_stop(struct Sana2If *s2i)
{
    Kprintf("[sana2if] %s\n", __func__);
    if (s2i->s2i_Pump == NULL)
        return;

    s2i->s2i_PumpWaiter = FindTask(NULL);
    SetSignal(0UL, SIGBREAKF_CTRL_F);
    Signal(&s2i->s2i_Pump->pr_Task, SIGBREAKF_CTRL_C);
    Wait(SIGBREAKF_CTRL_F);
    s2i->s2i_Pump = NULL;
}
