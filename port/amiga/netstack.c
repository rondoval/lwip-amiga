/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "netstack_sys.h"

#define TIMER_BASE_NAME TimerBase
#ifdef __INTELLISENSE__
#include <clib/timer_protos.h>
#else
#include <proto/timer.h>
#endif

#include <debug.h>

#include <lwip/init.h>
#include <lwip/sys.h>
#include <lwip/timeouts.h>

#include "netdev_if.h"
#include "netstack.h"

struct NetStack netstack;
struct Device *TimerBase;

void netstack_init(struct Device *timerBase)
{
    TimerBase = timerBase;

    InitSemaphore(&netstack.ns_Core);

    struct EClockVal ev;
    ULONG freq = ReadEClock(&ev);
    netstack.ns_EClockPerMs = freq / 1000;
    if (netstack.ns_EClockPerMs == 0)
        netstack.ns_EClockPerMs = 1;
    netstack.ns_LastEClockLo = ev.ev_lo;
    netstack.ns_RandState = ev.ev_lo | 1;

    netstack_lock();
    lwip_init();
    netstack_unlock();

    Kprintf("[netstack] initialized, eclock %lu Hz\n", freq);
}

void netstack_lock(void)
{
    ObtainSemaphore(&netstack.ns_Core);
}

void netstack_unlock(void)
{
    ReleaseSemaphore(&netstack.ns_Core);
}

void netstack_assert_locked(void)
{
#ifdef DEBUG
    if (netstack.ns_Core.ss_Owner != FindTask(NULL))
        Kprintf("[netstack] core lock NOT held by caller!\n");
#endif
}

void netstack_tick(void)
{
    netstack_lock();
    sys_check_timeouts();
    netstack_unlock();
}

/* Monotonic ms from the 32-bit EClock low word: unsigned wraparound delta,
 * divide-carry so remainder ticks are never lost. Correct as long as
 * successive calls are < ~1.6 h apart — the stack task ticks every 100 ms. */
ULONG netstack_now_ms(void)
{
    if (TimerBase == NULL)
        return 0;

    struct EClockVal ev;
    ReadEClock(&ev);

    ULONG delta = ev.ev_lo - netstack.ns_LastEClockLo;
    netstack.ns_LastEClockLo = ev.ev_lo;

    ULONG ticks = netstack.ns_TickRemainder + delta;
    netstack.ns_Ms += ticks / netstack.ns_EClockPerMs;
    netstack.ns_TickRemainder = ticks % netstack.ns_EClockPerMs;

    return netstack.ns_Ms;
}

/* lwIP's clock */
u32_t sys_now(void)
{
    return netstack_now_ms();
}

unsigned int netstack_lwip_rand(void)
{
    ULONG x = netstack.ns_RandState;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    netstack.ns_RandState = x;
    return x;
}

void netstack_platform_diag(const char *msg)
{
    Kprintf("[lwip] ASSERT: %s\n", (ULONG)msg);
}

/*
 * The lwIP heap. Every PBUF_RAM payload (= every TX frame) comes through
 * here, so packet memory is DMA-reachable whenever a NIC is attached: the
 * active netdev's allocator serves it. Non-packet heap users (DNS etc.)
 * and the pre-attach window fall back to AllocMem — nothing allocated then
 * is handed to hardware. An 8-byte header remembers size + origin so
 * netstack_free can route correctly even across an attach/detach.
 */

#define NSMEM_ORIGIN_EXEC 0x45584543UL /* 'EXEC' */
#define NSMEM_ORIGIN_DMA  0x444d4120UL /* 'DMA ' */

struct NsMemHeader
{
    ULONG nsm_Size; /* total, header included */
    ULONG nsm_Origin;
};

void *netstack_malloc(unsigned int size)
{
    ULONG total = (ULONG)size + sizeof(struct NsMemHeader);
    struct NetdevIf *nd = netstack.ns_ActiveNetdev;
    struct NsMemHeader *h;

    if (nd != NULL)
    {
        h = netdevif_dma_alloc(nd, total);
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

    if (h->nsm_Origin == NSMEM_ORIGIN_DMA)
        netdevif_dma_free(netstack.ns_ActiveNetdev, h, h->nsm_Size);
    else
        FreeMem(h, h->nsm_Size);
}
