/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * sana2_if — the lwIP netif over a classic SANA-II network device.
 *
 * The caller (the library's stack task, src/bsdsocket/sb_sana.c) owns the
 * exec side: it opens the device with sana2if_buffer_tags() as the
 * buffer-management tag list, runs the SANA-II bring-up commands
 * (S2_DEVICEQUERY/S2_GETSTATIONADDRESS/S2_CONFIGINTERFACE/S2_ONLINE), then
 * hands the results to sana2if_create(). The glue owns everything between
 * lwIP and the driver's IORequest surface, in COOKED mode (lwIP keeps
 * building/consuming full Ethernet frames; the 14-byte header is translated
 * at this boundary — RAW mode is unreliable across real drivers):
 *
 *   TX: linkoutput (under the core lock) parses the built Ethernet header
 *       into ios2_DstAddr/ios2_PacketType, stages a write request on a
 *       per-interface FIFO, and sana2if_tx_flush SendIO()s the batch at the
 *       outermost netstack_unlock — the netdev tx-kick idiom. The pbuf
 *       chain rides as the CopyFromBuff cookie (one extra ref, freed at
 *       completion); the driver copies from it, so the pbuf is never edited.
 *   RX: a per-interface pump task posts typed CMD_READs (IPv4/ARP, plus
 *       0x8100 under VLAN) whose CopyToBuff cookie points into a
 *       preallocated PBUF_RAM with 14 bytes of headroom; on completion the
 *       pump synthesizes the header, verifies TCP checksums off the lock
 *       (lwIP's own check is off — the GRO-merged headers must not be
 *       re-verified) and feeds ethernet_input through the shared GRO-lite
 *       merge (rx_gro.h) in batched core-lock holds. SANA-II is copy-based
 *       by construction — this is the compatibility backend, netdev
 *       remains the performance path.
 *
 * Locking: linkoutput and s2if_tx_complete run under the core lock; the
 * flush hook runs inside the outermost netstack_unlock (still under the
 * lock). The buffer callbacks are driver-called from unknown context and
 * touch no locks. The pump takes the lock only around lwIP entry.
 */

#ifndef LWIPAMIGA_SANA2_IF_H
#define LWIPAMIGA_SANA2_IF_H

#include <exec/ports.h>
#include <exec/types.h>
#include <utility/tagitem.h>

#include <lwip/netif.h>

#include "netif_base.h"
#include "rx_gro.h"

struct Device;
struct Unit;
struct Process;
struct S2TxReq;

/* Frames per pump core-lock hold (the netdev NDIF_RX_CHUNK analog): bounds
 * the requeue hold, the lwIP-input hold and the per-chunk GRO meta array below. */
#define S2IF_RX_BATCH 64

struct Sana2If
{
    struct NetIfBase s2i_Base; /* must stay first (netif_base.h) */

    /* exec identity for cloned IOSana2Reqs — the sanctioned duplication is
     * copying io_Device/io_Unit/ios2_BufferManagement from the opened
     * request; s2i_BufMgmt is the driver's per-opener cookie */
    struct Device *s2i_Device;
    struct Unit *s2i_Unit;
    APTR s2i_BufMgmt;
    ULONG s2i_Bps;  /* Sana2DeviceQuery.BPS, reported as link speed */
    UWORD s2i_Mtu;  /* netif MTU: config-clamped, -4 under in-band VLAN */
    UBYTE s2i_Mac[6];

    /* TX: fixed write-request pool + staged FIFO, mutated under the core
     * lock only. s2i_TxInFlight gates pump teardown (every SendIO'd write
     * replies to the pump port). */
    struct S2TxReq *s2i_TxFree;
    struct S2TxReq *s2i_TxStagedHead;
    struct S2TxReq **s2i_TxStagedTail;
    BOOL s2i_TxFlushPending;
    BOOL s2i_TxDown; /* teardown gate: linkoutput refuses new frames */
    ULONG s2i_TxInFlight;
    APTR s2i_TxStorage;
    ULONG s2i_TxStorageSize;

    /* RX pump (sana2_pump.c) */
    struct Process *s2i_Pump;
    struct MsgPort *s2i_PumpPort;  /* pump-owned: read/write/event replies */
    struct Task *s2i_PumpWaiter;   /* stack task blocked in start/stop */
    volatile LONG s2i_PumpRc;      /* pump startup result */

    /* glue counters (under the core lock). SANA-II global stats carry no
     * byte counts, so the glue counts them exactly — 64-bit as hi/lo. */
    ULONG s2i_RxBytesHi, s2i_RxBytesLo;
    ULONG s2i_TxBytesHi, s2i_TxBytesLo;
    ULONG s2i_TxErrors; /* write completed with io_Error set */
    ULONG s2i_TxDrops;  /* refused at linkoutput: pool dry / down / bad frame */
    ULONG s2i_RxErrors; /* read completed with an unexpected error */
    ULONG s2i_RxNoMem;  /* frame dropped: replacement pbuf alloc failed */
    ULONG s2i_RxCsumBad; /* pre-lock TCP verification failures (pump) */

    /* GRO-lite state (rx_gro.h) */
    struct RxGroMeta s2i_GroMeta[S2IF_RX_BATCH];
    struct RxGro s2i_Gro;
};

/* The buffer-management tag list to point ios2_BufferManagement at BEFORE
 * OpenDevice (plain S2_CopyToBuff/S2_CopyFromBuff; the 16/32/DMA variants
 * are advisory and not offered in v1). */
const struct TagItem *sana2if_buffer_tags(void);

/* Wire the bring-up results into lwIP: init the base, build the TX pool,
 * add the netif (down, unconfigured, link down). @mtu is the netif MTU
 * (already config-clamped, already -4 under VLAN); @hwMtu the driver's own
 * (RX buffer sizing + IFQ_HardwareMTU); @vlanTci -1 or (pcp<<13)|vid.
 * Returns 0 on success. */
LONG sana2if_create(struct Sana2If *s2i, struct Device *dev, struct Unit *unit,
                    APTR bufMgmt, const UBYTE mac[6], UWORD mtu, UWORD hwMtu,
                    ULONG bps, LONG vlanTci);

/* Tear down the lwIP side (netif removed, TX pool freed). The caller must
 * have stopped the pump first — all requests home, s2i_TxInFlight == 0. */
void sana2if_destroy(struct Sana2If *s2i);

/* Start/stop the RX pump task. Start posts the reads and arms the link
 * event before returning (ready handshake), so TX reply ports are valid —
 * call BEFORE netif_set_up (a static config's set_up emits a gratuitous
 * ARP). Stop aborts the reads, drains every outstanding request (writes
 * included) and frees the RX pbufs; after it returns the driver holds no
 * pointer of ours. */
LONG sana2if_pump_start(struct Sana2If *s2i);
void sana2if_pump_stop(struct Sana2If *s2i);

/* SendIO the staged TX batch. Called at every outermost netstack_unlock
 * (still under the lock); NULL-tolerant no-op when idle. */
void sana2if_tx_flush(struct Sana2If *s2i);

#endif /* LWIPAMIGA_SANA2_IF_H */
