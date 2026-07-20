/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The lwIP heap. Every PBUF_RAM payload (= every TX frame) comes through
 * here, so packet memory is DMA-reachable whenever a NIC is attached: the
 * active netdev's allocator serves it. Non-packet heap users (DNS etc.)
 * and the pre-attach window fall back to AllocMem — nothing allocated then
 * is handed to hardware. An 8-byte header remembers size + origin so
 * netstack_free can route correctly even across an attach/detach.
 *
 * Slab front-end: while a NIC is attached, three size classes serve all
 * packet-shaped allocations from O(1) intrusive freelists, replacing the
 * Exec Allocate()/Deallocate() first-fit walks the dma_mem pool pays per
 * call. All heap traffic runs under ns_Core, so the freelists need no
 * locking of their own. Slot sizes are cache-line multiples and arenas are
 * 64-byte aligned, so every slot owns whole cache lines — the driver's
 * pre-DMA clean never touches a neighbor's data. Only oversize requests
 * (> the class-2 slot) and the pre-attach window take the fallback paths.
 */

#include "netstack_sys.h"

#include <debug.h>

#include <lwip/opt.h>

#include "netdev_if.h"
#include "netstack.h"

#define NSMEM_ORIGIN_EXEC 0x45584543UL /* 'EXEC' */
#define NSMEM_ORIGIN_DMA  0x444d4120UL /* 'DMA ' */
#define NSMEM_ORIGIN_SLB0 0x534C4230UL /* 'SLB0' — slab class 0 */
#define NSMEM_ORIGIN_SLB1 0x534C4231UL /* 'SLB1' */
#define NSMEM_ORIGIN_SLB2 0x534C4232UL /* 'SLB2' */
#define NSMEM_ORIGIN_FREE 0x46524545UL /* 'FREE' — on a slab freelist */

struct NsMemHeader
{
    ULONG nsm_Size; /* total, header included (slab: the slot size) */
    ULONG nsm_Origin;
};

/* Slab classes, sized from the measured allocation census (totals include
 * the NsMemHeader): class 0 covers TCP control segments (bare ACK 84,
 * worst-case SACK ACK 124) and ip4_frag header templates; class 1 covers
 * the dominant class — full-MSS TCP pbufs (1544) and MTU UDP pbufs (1556)
 * — plus rare mid-size DHCP/DNS allocs riding along; class 2 exists for
 * the 64 KB UDP sendto pbuf (65592), the only legitimate size above class
 * 1 today, with two slots per arena (frag refs pin at most a few in
 * flight). Freed slots keep their arena's memory until detach — same
 * high-water retention model as the dma_mem puddles underneath. */
#define NSLAB_ARENA_HDR 64UL /* NsSlabArena, padded to keep slots 64-aligned */

struct NsSlabArena
{
    struct NsSlabArena *nsa_Next;
    ULONG nsa_Size;
};

static const ULONG nslab_slot[NS_SLAB_CLASSES] = {128, 1600, 65600};
static const ULONG nslab_arena_slots[NS_SLAB_CLASSES] = {64, 64, 2};
static const ULONG nslab_origin[NS_SLAB_CLASSES] = {
    NSMEM_ORIGIN_SLB0, NSMEM_ORIGIN_SLB1, NSMEM_ORIGIN_SLB2};

static BOOL nslab_grow(struct NetdevIf *nd, ULONG cls)
{
    ULONG slot = nslab_slot[cls];
    ULONG size = NSLAB_ARENA_HDR + nslab_arena_slots[cls] * slot;
    struct NsSlabArena *a = netdevif_dma_alloc(nd, size, 64);
    if (a == NULL)
        return FALSE; /* caller falls through to the one-off DMA path */

    a->nsa_Next = netstack.ns_SlabArenas[cls];
    a->nsa_Size = size;
    netstack.ns_SlabArenas[cls] = a;
    netstack.ns_SlabGrows[cls]++;

    UBYTE *s = (UBYTE *)a + NSLAB_ARENA_HDR;
    for (ULONG i = 0; i < nslab_arena_slots[cls]; i++, s += slot)
    {
        *(void **)s = netstack.ns_SlabFree[cls];
        netstack.ns_SlabFree[cls] = s;
    }
    Kprintf("[netstack] slab class %lu grew: %lu arena(s)\n",
            cls, netstack.ns_SlabGrows[cls]);
    return TRUE;
}

/* Return every arena to the driver pool and forget the freelists. Called
 * from netdevif_destroy under the core lock, BEFORE ns_ActiveNetdev is
 * cleared. Safe because shutdown is quiesced (destroy -> DETACH ->
 * CloseDevice, no lwIP calls in between): any block still in flight is
 * already lost either way, and its later free takes the leak-drop path in
 * netstack_free. Reattach hazard (multi-netif work, docs/TODO.md): a stale
 * slab-origin free after a second attach would silently push foreign
 * memory onto the new freelist — today the stack attaches exactly once. */
void netstack_slab_detach(struct NetdevIf *nd)
{
    for (ULONG cls = 0; cls < NS_SLAB_CLASSES; cls++)
    {
        struct NsSlabArena *a = netstack.ns_SlabArenas[cls];
        while (a != NULL)
        {
            struct NsSlabArena *next = a->nsa_Next;
            netdevif_dma_free(nd, a, a->nsa_Size);
            a = next;
        }
        netstack.ns_SlabArenas[cls] = NULL;
        netstack.ns_SlabFree[cls] = NULL;
    }
}

void *netstack_malloc(unsigned int size)
{
    ULONG asize = ((ULONG)size + 3) & ~3UL;
    ULONG total = asize + sizeof(struct NsMemHeader);
    struct NetdevIf *nd = netstack.ns_ActiveNetdev;
    struct NsMemHeader *h;

    if (nd != NULL && total <= nslab_slot[NS_SLAB_CLASSES - 1])
    {
        ULONG cls = (total <= nslab_slot[0]) ? 0UL
                  : (total <= nslab_slot[1]) ? 1UL
                                             : 2UL;
        void *slot = netstack.ns_SlabFree[cls];
        if (slot == NULL && nslab_grow(nd, cls))
            slot = netstack.ns_SlabFree[cls];
        if (slot != NULL)
        {
            netstack.ns_SlabFree[cls] = *(void **)slot;
            h = slot;
            h->nsm_Size = nslab_slot[cls];
            h->nsm_Origin = nslab_origin[cls];
            netstack.ns_MemInUse += h->nsm_Size;
            return h + 1;
        }
        /* grow failed: fall through — an alloc never fails on the slab */
    }

    if (nd != NULL)
    {
        h = netdevif_dma_alloc(nd, total, MEM_ALIGNMENT);
        if (h == NULL)
            return NULL;
        h->nsm_Origin = NSMEM_ORIGIN_DMA;
    }
    else
    {
        h = AllocMem(total, MEMF_PUBLIC);
        if (h == NULL)
            return NULL;
        h->nsm_Origin = NSMEM_ORIGIN_EXEC;
    }

    h->nsm_Size = total;
    netstack.ns_MemInUse += total;
    return h + 1;
}

void *netstack_calloc(unsigned int count, unsigned int size)
{
    ULONG bytes = (ULONG)count * size;
    void *p = netstack_malloc(bytes);
    if (p != NULL)
    {
        UBYTE *b = p;
        for (ULONG i = 0; i < bytes; i++)
            b[i] = 0;
    }
    return p;
}

void netstack_free(void *ptr)
{
    if (ptr == NULL)
        return;

    struct NsMemHeader *h = (struct NsMemHeader *)ptr - 1;
    netstack.ns_MemInUse -= h->nsm_Size;

    ULONG origin = h->nsm_Origin;
    ULONG size = h->nsm_Size;

    if (origin == NSMEM_ORIGIN_SLB0 || origin == NSMEM_ORIGIN_SLB1 ||
        origin == NSMEM_ORIGIN_SLB2)
    {
        if (netstack.ns_ActiveNetdev != NULL)
        {
            ULONG cls = origin - NSMEM_ORIGIN_SLB0;
            *(void **)h = netstack.ns_SlabFree[cls];
            netstack.ns_SlabFree[cls] = h;
        }
        else
        {
            /* arena already returned at detach: do NOT touch the block */
            Kprintf("[netstack] slab free after detach — leaked %lu bytes\n", size);
        }
        return;
    }

    if (origin == NSMEM_ORIGIN_DMA)
        netdevif_dma_free(netstack.ns_ActiveNetdev, h, size);
    else
        FreeMem(h, size);
}
