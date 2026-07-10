/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * netdev — NIC driver ABI for lwip-amiga (SANA-II replacement)
 *
 * Design principles (see planning/concept.md in the lwip-amiga repository):
 *  - Direct-call, context-based: no globals, every call carries an explicit
 *    context argument; drivers remain ROM-able. Lifecycle is OpenDevice() plus
 *    a direct-call attach handshake exchanging context pointers and callback
 *    tables with version/capability negotiation.
 *  - Split buffer ownership: the driver owns and recycles RX buffers (handed
 *    up as descriptors with a release hook); the stack owns its TX pool, drawn
 *    from a DMA allocator the driver provides at attach. DMA reachability is
 *    strictly driver-side knowledge.
 *  - Batched by design: arrays of descriptors per TX submit and per RX
 *    completion; buffer recycling is batched and foreign-task-safe.
 *  - Offloads negotiated at attach: TX/RX checksum (IPv4/TCP/UDP), interrupt
 *    coalescing hints; descriptor layout leaves room for more without an ABI
 *    break.
 *
 * This header is licensed BSD-2-Clause so that any driver or stack, under any
 * license, may implement the contract.
 *
 * Draft ABI lands in Phase 1. Until then this header only pins the name and
 * the version constant.
 */

#ifndef LWIPAMIGA_NETDEV_H
#define LWIPAMIGA_NETDEV_H

#define NETDEV_ABI_VERSION 0 /* pre-draft */

#endif /* LWIPAMIGA_NETDEV_H */
