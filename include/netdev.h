/* SPDX-License-Identifier: BSD-2-Clause */
/*
**  netdev.h — the netdev NIC driver ABI (SANA-II replacement), draft 1
**
**  The contract between a TCP/IP stack and a NIC driver on AmigaOS, designed
**  for zero-copy DMA and hardware offloads. This header is the entire ABI;
**  it is licensed BSD-2-Clause so any driver or stack, under any license,
**  may implement it. First implementation: lwip-amiga <-> genet.device.
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
**   - Stack -> driver entries (ndo_*) are callable from ANY task, never from
**     interrupts. They do not block. ndo_RxRelease and ndo_TxSubmit must be
**     safe to call concurrently from different tasks than the driver's own.
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
 * growth and new ops append and are gated by the negotiated version. */
#define NETDEV_ABI_VERSION      1

/* ------------------------------------------------------------------------ */
/* Commands — a reserved block in the NewStyle (NSCMD_*) pool.
 * The netdev block occupies NETDEV_CMD_BASE + 0x00..0x1f. */
#define NETDEV_CMD_BASE         0x4900

#define NETDEV_CMD_ATTACH       (NETDEV_CMD_BASE + 0x00)  /* NetDevAttach */
#define NETDEV_CMD_DETACH       (NETDEV_CMD_BASE + 0x01)  /* no payload */
#define NETDEV_CMD_START        (NETDEV_CMD_BASE + 0x02)  /* no payload */
#define NETDEV_CMD_STOP         (NETDEV_CMD_BASE + 0x03)  /* no payload */
#define NETDEV_CMD_SET_RXFILTER (NETDEV_CMD_BASE + 0x04)  /* NetDevRxFilter */
#define NETDEV_CMD_SET_COALESCE (NETDEV_CMD_BASE + 0x05)  /* NetDevCoalesce */
#define NETDEV_CMD_GET_STATS    (NETDEV_CMD_BASE + 0x06)  /* NetDevStats */
#define NETDEV_CMD_GET_LINK     (NETDEV_CMD_BASE + 0x07)  /* NetDevLinkState */
#define NETDEV_CMD_SET_MAC      (NETDEV_CMD_BASE + 0x08)  /* UBYTE[6] */

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
    ULONG   ndc_RxPoolBufs;     /* total driver RX buffers (ring + spares);
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
 * NDCF_TX_L4CSUM the flag must not be set. */

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
    APTR    ntd_Cookie;         /* returned verbatim by nso_TxDone */
};

#define NDTF_L4CSUM         (1 << 0)
#define NDTF_L4_UDP         (1 << 1)   /* with NDTF_L4CSUM: the L4 protocol is
                                          UDP, so the engine applies the UDP
                                          zero-checksum rewrite (0 -> 0xFFFF) */

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
    APTR    nrd_Cookie;         /* the buffer's handle for ndo_RxRelease */
};

#define NDRF_CSUM_VALID     (1 << 0)
#define NDRF_CSUM_RAW       (1 << 1)
#define NDRF_BCAST          (1 << 2)   /* informational */
#define NDRF_MCAST          (1 << 3)   /* informational */

/* ------------------------------------------------------------------------ */
/* The direct-call tables. Both stay valid from ATTACH to DETACH completion;
 * the driver's table is const (ROM-able), the stack's must outlive the
 * attachment. New entries append in future ABI versions. */

struct NetDevLinkState;         /* defined with NETDEV_CMD_GET_LINK below */

struct NetDevDrvOps             /* driver provides, stack calls */
{
    /* Submit a batch. Returns the number of descriptors accepted, from the
     * FRONT of the array (a short return means the TX ring is full — retry
     * the tail after the next nso_TxDone). One hardware doorbell per call.
     * Every accepted cookie returns via nso_TxDone exactly once. */
    LONG    (*ndo_TxSubmit)(APTR drvctx, const struct NetDevTxDesc *descs, ULONG count);

    /* Return one RX buffer to the driver. Foreign-task-safe and cheap (the
     * driver batches refills internally). */
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
 * the other again. */

struct NetDevAttach
{
    UWORD   nda_AbiVersion;     /* IN: stack's version; OUT: negotiated */
    UWORD   nda_Pad;
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
 * ATTACH. 64-bit big-endian counter pairs (no UQUAD in NDK 3.2). */

struct NetDevU64
{
    ULONG   ndu_Hi;
    ULONG   ndu_Lo;
};

struct NetDevStats
{
    struct NetDevU64 nds_RxPackets;
    struct NetDevU64 nds_RxBytes;
    struct NetDevU64 nds_RxErrors;      /* CRC/length/DMA errors, driver-dropped */
    struct NetDevU64 nds_RxDropped;     /* all software RX drops (pool dry + backpressure) */
    struct NetDevU64 nds_TxPackets;
    struct NetDevU64 nds_TxBytes;
    struct NetDevU64 nds_TxErrors;
    struct NetDevU64 nds_TxDropped;     /* completed-unsent on STOP */
    /* Loss-point split, each naming a distinct bottleneck: */
    ULONG   nds_RxOverruns;     /* ring full, HW discarded (drain too slow) */
    ULONG   nds_RxFifoOvfl;     /* RBUF FIFO overflow (ring not absorbing bursts) */
    ULONG   nds_RxPoolDry;      /* free pool empty (stack holding too many buffers) */
    ULONG   nds_RxBackpressure; /* nso_RxInput refused delivery */
    ULONG   nds_TxRejected;     /* TxSubmit accepted nothing (ring full) */
    ULONG   nds_TxBad;          /* descriptors dropped by the TX sanity gate */
    ULONG   nds_IrqRx;          /* RX-DONE interrupt count */
    ULONG   nds_RxPoolFree;     /* instantaneous free-pool gauge (NOT monotonic) */
    ULONG   nds_Reserved[8];
};

/* ------------------------------------------------------------------------ */
/* layout freeze */
NETDEV_ABI_ASSERT(sizeof(struct NetDevSg) == 8);
NETDEV_ABI_ASSERT(sizeof(struct NetDevTxDesc) == 16);
NETDEV_ABI_ASSERT(sizeof(struct NetDevRxDesc) == 16);
NETDEV_ABI_ASSERT(sizeof(struct NetDevCaps) == 40);
NETDEV_ABI_ASSERT(sizeof(struct NetDevAttach) == 60);
NETDEV_ABI_ASSERT(sizeof(struct NetDevRxFilter) == 8);
NETDEV_ABI_ASSERT(sizeof(struct NetDevCoalesce) == 8);
NETDEV_ABI_ASSERT(sizeof(struct NetDevLinkState) == 4);
NETDEV_ABI_ASSERT(sizeof(struct NetDevStats) == 128);

#if defined(__GNUC__)
#pragma pack()
#endif

#endif /* NETDEV_H */
