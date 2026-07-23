/* SPDX-License-Identifier: BSD-3-Clause */
/*
**  netdev.h — the netdev NIC driver ABI (SANA-II replacement), v1
**
**  The contract between a TCP/IP stack and a NIC driver on AmigaOS, designed
**  for zero-copy DMA and hardware offloads.
**
**  Request framing (the emu68 fleet idiom, cf. the xHCI context ABI):
**   - CONTROL OPS are synchronous commands on a plain struct IOStdReq:
**     io_Command = the NSCMD below, io_Data -> the op struct, io_Length =
**     its size, io_Error returns 0 or an NDERR_/IOERR_ code. OUT fields are
**     filled by the driver. Ops are serialized by the driver's unit task.
**     A driver advertises the commands it implements via NSCMD_DEVICEQUERY;
**     a dual-personality driver (SANA-II + netdev) lists both sets.
**   - The DATA PATH is direct calls in both directions, exchanged once by
**     NETDEV_CMD_ATTACH. Every entry carries an explicit context as its
**     first argument (drivers are ROM-able and carry no writable data — the
**     context is their only anchor). Plain C (stack-argument) calling
**     convention. No IORequest ever travels for a packet.
**
**  Lifecycle:
**     OpenDevice(unit, exclusive)
**       -> NETDEV_CMD_ATTACH        (exchange contexts, ops, capabilities)
**       -> control ops              (SET_RXFILTER, SET_COALESCE, ...)
**       -> NETDEV_CMD_START         (data path live)
**       ...
**       -> NETDEV_CMD_STOP          (quiesce, see below)
**       -> NETDEV_CMD_DETACH        (all buffers home, contexts die)
**     CloseDevice()
**     One attach per unit at a time; ATTACH fails with NDERR_BUSY while the
**     unit is attached or has active legacy (SANA-II) openers.
**
**  Buffer ownership — the founding rule (only the driver knows what its DMA
**  engine reaches; the stack NEVER AllocMem()s packet memory):
**   - RX buffers are DRIVER-owned. The driver allocates them, fills them by
**     DMA, hands them up via nso_RxInput() and gets each one back through
**     ndo_RxRelease(cookie). Buffer geometry (size, headroom, ring depth,
**     copy-break policy) is driver-internal and never crosses this ABI.
**     Only a COUNT crosses it: the stack states its concurrent-hold budget
**     at ATTACH (nda_RxHoldReq) and the driver answers with the enforced
**     bound (ndc_RxPoolBufs).
**   - TX memory is STACK-owned but comes from the driver's allocator
**     (ndo_DmaAlloc/ndo_DmaFree), which guarantees DMA reachability. Every
**     TX segment MUST lie in memory obtained from ndo_DmaAlloc of the same
**     unit OR inside an RX buffer of the same unit that the stack still
**     holds (echo/forwarding reuse an RX frame as TX data; both pools are
**     driver memory). The driver does not re-check reachability on the
**     fast path.
**
**  Cache coherency is entirely DRIVER-side: the driver performs the
**  CachePreDMA/CachePostDMA dance for both directions. The stack treats
**  packet memory as plain memory at all times.
**
**  Threading rules:
**   - Stack -> driver entries (ndo_*) are callable from ANY task (not just
**     the driver's own), never from interrupts. They do not block. The
**     CALLER serializes its own calls to ndo_RxRelease / ndo_TxSubmit — a
**     driver may recycle and submit through single-producer rings, so these
**     need not be re-entrant against themselves. The stack's core lock
**     provides that serialization; concurrent self-entry is not permitted.
**   - Driver -> stack callbacks (nso_*) are called ONLY from the driver's
**     unit task, never from interrupts, and must not block. A callback may
**     re-enter ndo_TxSubmit / ndo_RxRelease.
**   - Control ops may be issued from any task context via DoIO; they may
**     sleep (unit-task round trip) and must not be issued from callbacks.
*/

#ifndef NETDEV_H
#define NETDEV_H

#ifndef EXEC_TYPES_H
#include <exec/types.h>
#endif

#if defined(__GNUC__)
#pragma pack(2)
#endif

/* ------------------------------------------------------------------------ */
/* ABI version. NETDEV_CMD_ATTACH negotiates min(stack, driver); struct
 * growth and new ops append and are gated by the negotiated version.
 *
 * v1 carries reserved surface so the two open pre-freeze features can land
 * later WITHOUT another layout break:
 *   - Jumbo frames: nda_MtuReq/ndc_Mtu negotiate the MTU; NDCF_RX_SCATTER +
 *     NDRF_SOP/NDRF_EOP reserve multi-buffer RX for frames past a single
 *     descriptor. A v1 driver may still cap the MTU at 1500.
 *   - Hardware VLAN offload: ntd_VlanTci/nrd_VlanTci + NDTF_VLAN_INSERT/
 *     NDRF_VLAN_STRIPPED + NDCF_VLAN_TX_INSERT/NDCF_VLAN_RX_STRIP. A driver
 *     without these caps carries 802.1Q tags in-band (the stack tags in
 *     software); the reserved fields let a future NIC offload insert/strip.
 * All reserved bits/fields read 0 and are unimplemented in v1. */
#define NETDEV_ABI_VERSION      1

/* ------------------------------------------------------------------------ */
/* Commands — a block in the third-party command area. The NSD standard
 * keeps 0x4000-0x7FFF and 0xC000-0xFFFF for the OS; third parties get
 * 0x0000-0x3FFF and 0x8000-0xBFFF. Fleet allocations: nvme passthrough
 * 0x8020..0x8024, xhci context ops 0x8800..0x881f, netdev here.
 * The netdev block occupies NETDEV_CMD_BASE + 0x00..0x1f. */
#define NETDEV_CMD_BASE         0x8900

#define NETDEV_CMD_ATTACH       (NETDEV_CMD_BASE + 0x00)  /* NetDevAttach */
#define NETDEV_CMD_DETACH       (NETDEV_CMD_BASE + 0x01)  /* no payload */
#define NETDEV_CMD_START        (NETDEV_CMD_BASE + 0x02)  /* no payload */
#define NETDEV_CMD_STOP         (NETDEV_CMD_BASE + 0x03)  /* no payload */
#define NETDEV_CMD_SET_RXFILTER (NETDEV_CMD_BASE + 0x04)  /* NetDevRxFilter */
#define NETDEV_CMD_SET_COALESCE (NETDEV_CMD_BASE + 0x05)  /* NetDevCoalesce */
#define NETDEV_CMD_GET_STATS    (NETDEV_CMD_BASE + 0x06)  /* NetDevStats */
#define NETDEV_CMD_GET_LINK     (NETDEV_CMD_BASE + 0x07)  /* NetDevLinkState */
#define NETDEV_CMD_SET_MAC      (NETDEV_CMD_BASE + 0x08)  /* UBYTE[6] */
#define NETDEV_CMD_GET_COUNTERS (NETDEV_CMD_BASE + 0x09)  /* NetDevCounterSet */

#define NETDEV_IS_CMD(cmd)      ((((UWORD)(cmd)) & 0xFFE0) == NETDEV_CMD_BASE)

/* io_Error codes (alongside the standard IOERR_ pool) */
#define NDERR_BADVERSION        40  /* no common ABI version */
#define NDERR_BUSY              41  /* unit attached or held by legacy openers */
#define NDERR_NOTATTACHED       42  /* op requires an attached unit */
#define NDERR_BADPARAMS         43
#define NDERR_NOMEM             44

/* layout freeze helpers (pack(2)) */
#define NETDEV_ABI_ASSERT2(cond, line) typedef char netdev_abi_assert_##line[(cond) ? 1 : -1]
#define NETDEV_ABI_ASSERT1(cond, line) NETDEV_ABI_ASSERT2(cond, line)
#define NETDEV_ABI_ASSERT(cond)        NETDEV_ABI_ASSERT1(cond, __LINE__)

/* ------------------------------------------------------------------------ */
/* Capabilities, filled by the driver at ATTACH. The stack enables only what
 * is advertised; absent features degrade (no TX csum -> stack computes in
 * software; no MCAST_FILTER -> driver goes all-multi when a list is set). */

struct NetDevCaps
{
    UWORD   ndc_AbiVersion;     /* the driver's highest supported version */
    UWORD   ndc_Mtu;            /* max payload MTU (1500 unless jumbo) */
    UBYTE   ndc_Mac[6];         /* factory/current station address */
    UWORD   ndc_TxMaxSegs;      /* max scatter-gather segments per packet */
    ULONG   ndc_Features;       /* NDCF_* */
    UWORD   ndc_TxRingSlots;    /* informational: hardware TX ring depth */
    UWORD   ndc_RxRingSlots;    /* informational: hardware RX ring depth */
    UWORD   ndc_TxAlign;        /* required TX segment address alignment in
                                   bytes (0/1 = none). ndo_DmaAlloc output
                                   always satisfies it; this matters when the
                                   stack points segments INTO such a buffer */
    UWORD   ndc_Pad;
    ULONG   ndc_RxPoolBufs;     /* total driver RX buffers (ring + spares),
                                   sized at ATTACH from ring depth plus the
                                   stack's nda_RxHoldReq (driver-clamped);
                                   bounds how many the stack may hold — it
                                   sizes its wrap storage from this */
    ULONG   ndc_Reserved[3];    /* 0 until a future ABI version claims them */
};

#define NDCF_TX_L4CSUM      (1UL << 0)  /* TX checksum insertion (NDTF_L4CSUM) */
#define NDCF_RX_CSUM_VALID  (1UL << 1)  /* RX csum verified, NDRF_CSUM_VALID */
#define NDCF_RX_CSUM_RAW    (1UL << 2)  /* RX raw csum reported, NDRF_CSUM_RAW */
#define NDCF_COALESCE       (1UL << 3)  /* SET_COALESCE is honored */
#define NDCF_MCAST_FILTER   (1UL << 4)  /* exact multicast filtering in HW */
#define NDCF_LINK_EVENTS    (1UL << 5)  /* nso_LinkChange will be called */
/* Reserved capabilities — defined so the frozen v1 layout can express them later without an
 * ABI break; no v1 driver advertises them and no v1 stack acts on them. */
#define NDCF_RX_SCATTER     (1UL << 6)  /* reserved: multi-buffer RX frames (see NDRF_SOP/EOP) */
#define NDCF_VLAN_TX_INSERT (1UL << 7)  /* reserved: HW inserts ntd_VlanTci (see NDTF_VLAN_INSERT).
                                           Absent = the stack tags in software (in-band 802.1Q) */
#define NDCF_VLAN_RX_STRIP  (1UL << 8)  /* reserved: HW strips the tag into nrd_VlanTci (see
                                           NDRF_VLAN_STRIPPED). Absent = tag stays in the frame */

/* ------------------------------------------------------------------------ */
/* TX descriptors. A packet is a scatter-gather list of segments, each in
 * memory from ndo_DmaAlloc. The descriptor ARRAY and the NetDevSg arrays are
 * only read during the ndo_TxSubmit call; the segment DATA must stay valid
 * and untouched until the packet's cookie returns via nso_TxDone. Segments
 * carry a complete Ethernet frame (dst/src/type already built) without FCS.
 *
 * NDTF_L4CSUM asks the hardware to compute a 16-bit one's-complement sum
 * over [ntd_CsumStart .. end of frame) and insert it at ntd_CsumOffset
 * (both byte offsets from the start of the frame). The stack pre-seeds the
 * 16-bit field at ntd_CsumOffset with the one's-complement sum of the L3
 * pseudo-header (Linux CHECKSUM_PARTIAL semantics — what offset-based
 * engines such as GENET's TSB require). This matches TCP/UDP over
 * IPv4/IPv6. The IPv4 header checksum is NOT offloaded — the stack
 * computes it (20 bytes, cheap, and lwIP does it inline). Without
 * NDCF_TX_L4CSUM the flag must not be set.
 *
 * ntd_CsumStart/ntd_CsumOffset are absolute byte offsets from the frame
 * start, so when the frame carries an in-band 802.1Q tag they already
 * include the 4-byte tag (the L4 position is simply shifted): the offset-
 * based engine needs no VLAN awareness. */

struct NetDevSg
{
    APTR    nsg_Data;
    ULONG   nsg_Len;
};

struct NetDevTxDesc
{
    const struct NetDevSg *ntd_Segs;
    UWORD   ntd_NumSegs;        /* 1..ndc_TxMaxSegs */
    UWORD   ntd_Flags;          /* NDTF_* */
    UWORD   ntd_CsumStart;      /* valid with NDTF_L4CSUM */
    UWORD   ntd_CsumOffset;     /* valid with NDTF_L4CSUM */
    UWORD   ntd_VlanTci;        /* reserved: 802.1Q TCI to insert with NDTF_VLAN_INSERT */
    APTR    ntd_Cookie;         /* returned verbatim by nso_TxDone */
};

#define NDTF_L4CSUM         (1 << 0)
#define NDTF_L4_UDP         (1 << 1)   /* with NDTF_L4CSUM: the L4 protocol is
                                          UDP, so the engine applies the UDP
                                          zero-checksum rewrite (0 -> 0xFFFF) */
#define NDTF_VLAN_INSERT    (1 << 2)   /* reserved (NDCF_VLAN_TX_INSERT): the driver inserts the
                                          802.1Q tag ntd_VlanTci. Unused in v1 — the stack builds
                                          tagged frames in software, so segments already carry the
                                          tag and this flag is never set. */

/* ------------------------------------------------------------------------ */
/* RX descriptors, delivered by the driver to nso_RxInput. nrd_Data points at
 * a complete received Ethernet frame (FCS stripped, already CachePostDMA'd,
 * any hardware prefix such as a status block already skipped). The
 * descriptor array is valid only for the duration of the call; the DATA and
 * the cookie stay valid until the stack returns the buffer with
 * ndo_RxRelease(cookie). Consumed descriptors transfer ownership; refused
 * ones (see nso_RxInput) stay with the driver and are recycled immediately.
 *
 * Checksum reporting, by driver capability:
 *   NDRF_CSUM_VALID — the driver verified the L4 checksum; the stack skips
 *                     its software check.
 *   NDRF_CSUM_RAW   — nrd_CsumRaw is the raw 16-bit one's-complement sum
 *                     over the frame past the Ethernet header; the stack
 *                     folds it against the pseudo-header itself.
 * Neither flag set on a frame = checksum unknown, stack checks in software
 * (always the case for drivers without RX csum features). */

struct NetDevRxDesc
{
    APTR    nrd_Data;
    ULONG   nrd_Len;
    UWORD   nrd_Flags;          /* NDRF_* */
    UWORD   nrd_CsumRaw;        /* valid with NDRF_CSUM_RAW */
    UWORD   nrd_VlanTci;        /* reserved: 802.1Q TCI stripped by HW with NDRF_VLAN_STRIPPED */
    APTR    nrd_Cookie;         /* the buffer's handle for ndo_RxRelease */
};

#define NDRF_CSUM_VALID     (1 << 0)
#define NDRF_CSUM_RAW       (1 << 1)
#define NDRF_BCAST          (1 << 2)   /* informational */
#define NDRF_MCAST          (1 << 3)   /* informational */
/* Reserved RX flags — unimplemented in v1 (see the reserved NDCF_* capabilities). */
#define NDRF_VLAN_STRIPPED  (1 << 4)   /* reserved (NDCF_VLAN_RX_STRIP): HW stripped the 802.1Q
                                          tag into nrd_VlanTci. Unused in v1 — the tag stays in
                                          the frame and the stack parses it. */
#define NDRF_SOP            (1 << 5)   /* reserved (NDCF_RX_SCATTER): start-of-frame buffer */
#define NDRF_EOP            (1 << 6)   /* reserved (NDCF_RX_SCATTER): end-of-frame buffer. v1
                                          delivers one buffer per frame (implicit SOP+EOP); a
                                          future version may deliver a frame as a chain of
                                          buffers, only the last carrying NDRF_EOP. */

/* ------------------------------------------------------------------------ */
/* The direct-call tables. Both stay valid from ATTACH to DETACH completion;
 * the driver's table is const (ROM-able), the stack's must outlive the
 * attachment. New entries append in future ABI versions. */

struct NetDevLinkState;         /* defined with NETDEV_CMD_GET_LINK below */

struct NetDevDrvOps             /* driver provides, stack calls */
{
    /* Submit a batch. Returns the number of descriptors accepted, from the
     * FRONT of the array (a short return means the TX ring is full — retry
     * the tail after the next nso_TxDone). Stages the descriptors on the ring
     * but does NOT ring the hardware doorbell; the stack must call ndo_TxKick
     * to publish them (one doorbell per ndo_TxKick, not per submit).
     * Every accepted cookie returns via nso_TxDone exactly once. */
    LONG    (*ndo_TxSubmit)(APTR drvctx, const struct NetDevTxDesc *descs, ULONG count);

    /* Ring the batched TX doorbell: publish every descriptor staged by prior
     * ndo_TxSubmit calls since the last kick, so the hardware starts reading
     * them. The stack calls this once per locked burst — collapsing N
     * per-frame doorbells into one. Required (a no-op if nothing was staged). */
    VOID    (*ndo_TxKick)(APTR drvctx);

    /* Return one RX buffer to the driver. Callable from any task and cheap
     * (the driver batches refills internally); the caller serializes its
     * own calls — a single-producer recycle ring is a valid implementation
     * (see the threading rules above). */
    VOID    (*ndo_RxRelease)(APTR drvctx, APTR cookie);

    /* The TX memory allocator: DMA-reachable for this unit, aligned to at
     * least max(align, ndc_TxAlign). All allocations must be returned with
     * ndo_DmaFree (same size) before DETACH. */
    APTR    (*ndo_DmaAlloc)(APTR drvctx, ULONG size, ULONG align);
    VOID    (*ndo_DmaFree)(APTR drvctx, APTR ptr, ULONG size);
};

struct NetDevStackOps           /* stack provides, driver calls */
{
    /* A batch of received frames. Returns how many descriptors the stack
     * consumed, from the FRONT of the array; ownership of those transfers
     * to the stack (returned later via ndo_RxRelease). A short return is
     * backpressure: the driver recycles the tail immediately and counts
     * them as nds_RxDropped. */
    ULONG   (*nso_RxInput)(APTR stackctx, const struct NetDevRxDesc *descs, ULONG count);

    /* Completed TX cookies (transmitted or dropped on STOP). The stack may
     * reclaim the segment memory and submit queued packets from here. */
    VOID    (*nso_TxDone)(APTR stackctx, APTR const *cookies, ULONG count);

    /* Link state changed (only with NDCF_LINK_EVENTS). Also called once
     * shortly after START with the then-current state. */
    VOID    (*nso_LinkChange)(APTR stackctx, const struct NetDevLinkState *state);
};

/* ------------------------------------------------------------------------ */
/* NETDEV_CMD_ATTACH (io_Data -> struct NetDevAttach, io_Length = sizeof).
 *
 * The stack proposes its highest ABI version; the driver replies with
 * min(stack, driver) in nda_AbiVersion and fills the OUT fields, or fails
 * with NDERR_BADVERSION / NDERR_BUSY. After success the data path exists
 * but is quiescent until NETDEV_CMD_START.
 *
 * NETDEV_CMD_STOP quiesces: RX delivery ceases, every in-flight TX cookie is
 * completed via nso_TxDone (transmitted or not) — all before the op replies.
 * RX buffers held by the stack stay valid; the stack releases them at its
 * own pace. NETDEV_CMD_DETACH requires: stopped, all RX cookies released,
 * all ndo_DmaAlloc memory freed. After DETACH replies, neither side calls
 * the other again.
 *
 * RX pool sizing: the stack may pin one full receive window per bulk
 * stream in socket queues, so its hold budget is roughly
 *   streams x (TCP_WND / TCP_MSS) + in-flight slack.
 * The driver adds its RX ring depth and recycle-latency slack, clamps to
 * its own bounds, allocates the pool during ATTACH, and reports the
 * result in ndc_RxPoolBufs. */

struct NetDevAttach
{
    UWORD   nda_AbiVersion;     /* IN: stack's version; OUT: negotiated */
    UWORD   nda_RxHoldReq;      /* IN: max RX buffers the stack intends to
                                   hold concurrently (its receive-queue
                                   budget); 0 = no estimate. The driver
                                   covers ring + this in its RX pool and
                                   reports the enforced bound in
                                   ndc_RxPoolBufs */
    UWORD   nda_MtuReq;         /* IN: stack's desired payload MTU, 0 = driver
                                   default. The driver clamps to what its
                                   buffers support and returns the actual value
                                   in ndc_Mtu (OUT). Mirrors the nda_RxHoldReq
                                   -> ndc_RxPoolBufs negotiation. */
    APTR    nda_StackCtx;       /* IN: first arg of every nso_* call */
    const struct NetDevStackOps *nda_StackOps;  /* IN */
    APTR    nda_DrvCtx;         /* OUT: first arg of every ndo_* call */
    const struct NetDevDrvOps *nda_DrvOps;      /* OUT */
    struct NetDevCaps nda_Caps; /* OUT */
};

/* ------------------------------------------------------------------------ */
/* NETDEV_CMD_SET_RXFILTER (io_Data -> struct NetDevRxFilter).
 * Declarative set-state (not deltas): the driver programs exactly this
 * reception state. Unicast-to-us and broadcast are always received. A
 * driver whose exact-match filter cannot hold ndrx_NumMcast entries falls
 * back to all-multi reception internally. The list is only read during the
 * op. Default state after ATTACH: no multicast, no promisc. */

struct NetDevRxFilter
{
    UWORD   ndrx_Flags;         /* NDFF_* */
    UWORD   ndrx_NumMcast;
    const UBYTE (*ndrx_McastList)[6];
};

#define NDFF_PROMISC        (1 << 0)
#define NDFF_ALLMULTI       (1 << 1)

/* NETDEV_CMD_SET_COALESCE (io_Data -> struct NetDevCoalesce). Interrupt
 * moderation hints; 0 = driver default. Honored with NDCF_COALESCE,
 * otherwise a successful no-op. */

struct NetDevCoalesce
{
    ULONG   ndcl_RxUsecs;       /* max delay before an RX interrupt */
    UWORD   ndcl_RxMaxFrames;   /* ... or after this many frames */
    UWORD   ndcl_TxMaxFrames;   /* TX-done interrupt batching */
};

/* NETDEV_CMD_GET_LINK (io_Data -> struct NetDevLinkState) — poll variant of
 * nso_LinkChange. */

struct NetDevLinkState
{
    UWORD   ndls_Flags;         /* NDLF_* */
    UWORD   ndls_SpeedMbps;     /* 10/100/1000; 0 when down */
};

#define NDLF_UP             (1 << 0)
#define NDLF_FULL_DUPLEX    (1 << 1)

/* NETDEV_CMD_GET_STATS (io_Data -> struct NetDevStats). Monotonic since
 * ATTACH. 64-bit big-endian counter pairs. */

struct NetDevU64
{
    ULONG   ndu_Hi;
    ULONG   ndu_Lo;
};

/* The ABI stores 64-bit counters as a big-endian hi/lo ULONG pair. These are
 * the sanctioned way to cross that split, on both sides of the boundary. */
static inline void netdev_u64_set(struct NetDevU64 *out, unsigned long long value)
{
    out->ndu_Hi = (ULONG)(value >> 32);
    out->ndu_Lo = (ULONG)value;
}

static inline unsigned long long netdev_u64_get(const struct NetDevU64 *v)
{
    return ((unsigned long long)v->ndu_Hi << 32) | (unsigned long long)v->ndu_Lo;
}

struct NetDevStats
{
    struct NetDevU64 nds_RxPackets;
    struct NetDevU64 nds_RxBytes;
    struct NetDevU64 nds_RxErrors;      /* CRC/length/DMA errors, driver-dropped */
    struct NetDevU64 nds_RxDropped;     /* all software RX drops (pool dry + backpressure) */
    struct NetDevU64 nds_TxPackets;
    struct NetDevU64 nds_TxBytes;
    struct NetDevU64 nds_TxErrors;
    struct NetDevU64 nds_TxDropped;     /* accepted from the stack, never sent —
                                           whatever the reason (quiesce, a
                                           rejected descriptor, ...) */
    /* Hardware loss, which the stack has no other way of learning. Both are
     * frames the MAC saw and the driver never did; they are separate because
     * they fail in different places and point at different fixes. */
    ULONG   nds_RxOverruns;     /* ring full, HW discarded (drain too slow) */
    ULONG   nds_RxFifoOvfl;     /* RBUF FIFO overflow (ring not absorbing bursts) */
    ULONG   nds_Reserved[8];
};

/* NETDEV_CMD_GET_COUNTERS (io_Data -> struct NetDevCounterSet).
 *
 * The open-ended companion to NetDevStats: that struct is the fixed, portable
 * set consumed programmatically (bsdsocket's interface query maps named fields
 * onto it), this one is whatever diagnostic counters the driver happens to
 * have, each carrying its own name so a renderer needs no per-driver
 * knowledge.
 *
 * A driver is expected to report a counter here even when NetDevStats
 * already has a field for it.
 *
 * Optional: a driver without it answers IOERR_NOCMD, and NSCMD_DEVICEQUERY
 * lists the command when it is present.
 *
 * Two-call sizing, like NSCMD_DEVICEQUERY: call with ndcs_Max 0 to learn
 * ndcs_Count, size the buffer with NETDEV_COUNTERSET_SIZE, call again. The
 * driver fills min(Max, Count) entries and always reports the true Count.
 */

/* ndcn_Flags */
#define NDCNTF_GAUGE    (1 << 0)    /* instantaneous, not monotonic: no rate */
#define NDCNTF_WRAP32   (1 << 1)    /* free-running 32-bit hardware counter:
                                       difference it, never total it (a MAC
                                       byte counter wraps in ~34 s at 1 Gb/s) */
#define NDCNTF_ERROR    (1 << 2)    /* loss/error: a renderer may highlight it */

struct NetDevCounter
{
    struct NetDevU64 ndcn_Value;
    CONST_STRPTR     ndcn_Name;     /* driver rodata; valid while the unit is
                                       open — the caller must not free it */
    UWORD            ndcn_Flags;    /* NDCNTF_* */
    UWORD            ndcn_Pad;
};

struct NetDevCounterSet
{
    UWORD   ndcs_Max;       /* [in]  entries ndcs_Counters can hold */
    UWORD   ndcs_Count;     /* [out] entries the driver has; may exceed ndcs_Max */
    struct NetDevCounter ndcs_Counters[1];   /* [out] min(Max, Count) entries */
};

#define NETDEV_COUNTERSET_SIZE(n)   (4UL + (ULONG)(n) * 16UL)

/* ------------------------------------------------------------------------ */
/* layout freeze */
NETDEV_ABI_ASSERT(sizeof(struct NetDevSg) == 8);
NETDEV_ABI_ASSERT(sizeof(struct NetDevTxDesc) == 18);
NETDEV_ABI_ASSERT(sizeof(struct NetDevRxDesc) == 18);
NETDEV_ABI_ASSERT(sizeof(struct NetDevCaps) == 40);
NETDEV_ABI_ASSERT(sizeof(struct NetDevAttach) == 62);
NETDEV_ABI_ASSERT(sizeof(struct NetDevRxFilter) == 8);
NETDEV_ABI_ASSERT(sizeof(struct NetDevCoalesce) == 8);
NETDEV_ABI_ASSERT(sizeof(struct NetDevLinkState) == 4);
NETDEV_ABI_ASSERT(sizeof(struct NetDevStats) == 104);
NETDEV_ABI_ASSERT(sizeof(struct NetDevCounter) == 16);
NETDEV_ABI_ASSERT(sizeof(struct NetDevCounterSet) == NETDEV_COUNTERSET_SIZE(1));

#if defined(__GNUC__)
#pragma pack()
#endif

#endif /* NETDEV_H */
