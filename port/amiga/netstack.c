/* SPDX-License-Identifier: BSD-3-Clause */

#include "netstack_sys.h"

#define TIMER_BASE_NAME TimerBase
#ifdef __INTELLISENSE__
#include <clib/timer_protos.h>
#else
#include <proto/timer.h>
#endif

#include <debug.h>
#include <timing.h> /* get_time(): BCM 1 MHz system timer, lock profiling */

#include <lwip/init.h>
#include <lwip/netif.h>
#include <lwip/sys.h>
#include <lwip/timeouts.h>

#include "netdev_if.h"
#include "netstack.h"
#include "nsprof.h"

struct NetStack netstack;
struct Device *TimerBase;

/* The netstack perf instance (emu68-common <perf.h>). Slot storage is
 * unconditional so DEBUG and release share one layout; order of the name
 * table must match enum NsProfSlot. */
static struct perf_counter ns_perf_slots[NSP_SLOT_COUNT];
static const char *const ns_perf_names[NSP_SLOT_COUNT] = {
    "rx_csum",   "rx_lockwait",   "rx_input",   "rx_gro",
    "tx_done",
    "tx_linkout", "tx_submit",
    "recv_lockwait", "recv_copy", "recv_ackflush", "recv_sleep",
    "send_lockwait", "send_write", "send_output", "send_sleep",
    "udp_send",
};
struct perf ns_perf = { "nsprof", ns_perf_names, ns_perf_slots, NSP_SLOT_COUNT };

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
#ifdef DEBUG
    /* wait/hold split for the CPU-ceiling hunt; outermost holds only */
    u32 t0 = get_time();
    ObtainSemaphore(&netstack.ns_Core);
    if (netstack.ns_Core.ss_NestCount == 1)
    {
        u32 t1 = get_time();
        netstack.ns_LockWaitUs += t1 - t0;
        netstack.ns_LockT0 = t1;
        netstack.ns_LockHolds++;
    }
#else
    ObtainSemaphore(&netstack.ns_Core);
#endif
}

static BOOL netstack_loopback_pending(void)
{
    struct netif *nif;
    NETIF_FOREACH(nif)
    {
        if (nif->loop_first != NULL)
            return TRUE;
    }
    return FALSE;
}

void netstack_unlock(void)
{
    /* Loopback delivery. NO_SYS=1 means nothing schedules netif_poll():
     * every packet lwIP routes to 127.0.0.1 (or a netif's own address)
     * just sits on that netif's loop queue. Drain it at the outermost
     * unlock — still under the lock, in whatever context produced the
     * packets — so loopback traffic completes synchronously. netif_poll()
     * drains same-netif chains itself; the outer while covers packets a
     * drained one enqueues on another netif. */
    if (netstack.ns_Core.ss_NestCount == 1)
    {
        while (netstack_loopback_pending())
            netif_poll_all();
    }
#ifdef DEBUG
    if (netstack.ns_Core.ss_NestCount == 1)
    {
        u32 dt = get_time() - netstack.ns_LockT0;
        netstack.ns_LockHoldUs += dt;
        if (dt > netstack.ns_LockHoldMaxUs)
            netstack.ns_LockHoldMaxUs = dt;
    }
#endif
    ReleaseSemaphore(&netstack.ns_Core);
}

void netstack_assert_locked(void)
{
    // KprintfH("[netstack] %s\n", __func__);
#ifdef DEBUG
    if (netstack.ns_Core.ss_Owner != FindTask(NULL))
        Kprintf("[netstack] core lock NOT held by caller!\n");
#endif
}

void netstack_tick(void)
{
    // KprintfH("[netstack] %s\n", __func__);
    netstack_lock();
    sys_check_timeouts();
#ifdef DEBUG
    /* lock-profile line every ~2 s (20 × 100 ms ticks) when anything held */
    if (++netstack.ns_LockProfTicks >= 20)
    {
        netstack.ns_LockProfTicks = 0;
        if (netstack.ns_LockHolds != 0)
        {
            Kprintf("[netstack] lock: %lu holds, wait %lu us, hold %lu us, maxhold %lu us\n",
                    (ULONG)netstack.ns_LockHolds, (ULONG)netstack.ns_LockWaitUs,
                    (ULONG)netstack.ns_LockHoldUs, (ULONG)netstack.ns_LockHoldMaxUs);
            netstack.ns_LockHolds = 0;
            netstack.ns_LockWaitUs = 0;
            netstack.ns_LockHoldUs = 0;
            netstack.ns_LockHoldMaxUs = 0;
        }
        perf_report(&ns_perf);
    }
#endif
    netstack_unlock();
}

/* Monotonic ms from the 32-bit EClock low word: unsigned wraparound delta,
 * divide-carry so remainder ticks are never lost. Correct as long as
 * successive calls are < ~1.6 h apart — the stack task ticks every 100 ms. */
ULONG netstack_now_ms(void)
{
    // KprintfH("[netstack] %s: ms %lu\n", __func__, netstack.ns_Ms);
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
    // KprintfH("[netstack] %s\n", __func__);
    return netstack_now_ms();
}

unsigned int netstack_lwip_rand(void)
{
    // KprintfH("[netstack] %s\n", __func__);
    ULONG x = netstack.ns_RandState;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    netstack.ns_RandState = x;
    return x;
}

/* LWIP_PLATFORM_ASSERT. lwIP's LWIP_ASSERT assumes this never returns —
 * returning lets the core continue into state it has declared impossible
 * (observed 2026-07-14: "mss_local is too small" followed by a wild write
 * and a wedged stack). Freezing the calling task keeps the failed state
 * intact for post-mortem; other tasks then park on ns_Core, which is a
 * deterministic stall instead of corruption. The message is latched for
 * release builds, where Kprintf compiles out. */
const char *netstack_assert_msg;

void netstack_platform_diag(const char *msg)
{
    netstack_assert_msg = msg;
    Kprintf("[lwip] ASSERT: %s — task '%s' halted\n", (ULONG)msg,
            (ULONG)FindTask(NULL)->tc_Node.ln_Name);
    for (;;)
        Wait(0UL);
}

#ifdef DEBUG
#include <stddef.h> /* size_t, for the freestanding snprintf below */

/* lwIP debug output (LWIP_PLATFORM_DIAG). lwIP's format strings pass 32-bit
 * values as plain %d/%u/%x, which RawDoFmt would read as 16-bit — so the
 * formatting happens here with C argument promotion and only the finished
 * string reaches the backend. Covers the conversions lwIP uses (c s p d i
 * u x X, with -/0/width/h/l/z); anything else prints literally. */
#define NS_DIAG_BUF 256

static void ns_diag_putc(char *buf, ULONG *pos, ULONG cap, char c)
{
    if (*pos < cap - 1)
        buf[(*pos)++] = c;
}

/* The shared formatter: writes at most cap-1 chars plus a NUL and returns the
 * length written. Backs both netstack_diag_printf and the freestanding
 * snprintf below. */
static ULONG ns_vformat(char *buf, ULONG cap, const char *fmt, va_list ap)
{
    ULONG pos = 0;
    if (cap == 0)
        return 0;
    for (; *fmt != '\0'; fmt++)
    {
        if (*fmt != '%')
        {
            ns_diag_putc(buf, &pos, cap, *fmt);
            continue;
        }

        fmt++;
        char pad = ' ';
        if (*fmt == '-')
            fmt++; /* left-align: not worth honoring in a log */
        if (*fmt == '0')
        {
            pad = '0';
            fmt++;
        }
        ULONG width = 0;
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (ULONG)(*fmt++ - '0');
        while (*fmt == 'h' || *fmt == 'l' || *fmt == 'z')
            fmt++; /* every integer arrives 32-bit after promotion */

        char conv = *fmt;
        if (conv == '\0')
            break;
        if (conv == '%')
        {
            ns_diag_putc(buf, &pos, cap, '%');
            continue;
        }
        if (conv == 'c')
        {
            ns_diag_putc(buf, &pos, cap, (char)va_arg(ap, int));
            continue;
        }
        if (conv == 's')
        {
            const char *s = va_arg(ap, const char *);
            if (s == NULL)
                s = "(null)";
            for (; *s != '\0'; s++)
                ns_diag_putc(buf, &pos, cap, *s);
            continue;
        }

        ULONG val;
        ULONG base;
        const char *dig = (conv == 'X') ? "0123456789ABCDEF" : "0123456789abcdef";
        BOOL negative = FALSE;
        switch (conv)
        {
        case 'd':
        case 'i':
        {
            LONG sv = va_arg(ap, LONG);
            negative = sv < 0;
            val = negative ? (ULONG)0 - (ULONG)sv : (ULONG)sv;
            base = 10;
            break;
        }
        case 'u':
            val = va_arg(ap, ULONG);
            base = 10;
            break;
        case 'x':
        case 'X':
            val = va_arg(ap, ULONG);
            base = 16;
            break;
        case 'p':
            val = (ULONG)va_arg(ap, void *);
            base = 16;
            pad = '0';
            width = 8;
            break;
        default:
            /* unknown conversion: emit literally, don't touch the va_list */
            ns_diag_putc(buf, &pos, cap, '%');
            ns_diag_putc(buf, &pos, cap, conv);
            continue;
        }

        char tmp[11]; /* 32-bit decimal maxes at 10 digits, +1 for '-' */
        ULONG n = 0;
        do
        {
            tmp[n++] = dig[val % base];
            val /= base;
        } while (val != 0);
        if (negative)
            tmp[n++] = '-';
        for (; width > n; width--)
            ns_diag_putc(buf, &pos, cap, pad);
        while (n > 0)
            ns_diag_putc(buf, &pos, cap, tmp[--n]);
    }

    buf[pos] = '\0';
    return pos;
}

void netstack_diag_printf(const char *fmt, ...)
{
    char buf[NS_DIAG_BUF];
    va_list ap;

    va_start(ap, fmt);
    ns_vformat(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    Kprintf("%s", buf);
}

/* Freestanding snprintf. lwIP's debug builds format an assert message with
 * snprintf (MEMP_OVERFLOW_CHECK, enabled by DEBUG_HIGH); pulling libnix's
 * stdio to satisfy it fails the link, because a ROM-able library has no C
 * startup to supply SysBase/exit. This covers only the small format subset
 * lwIP uses — it is not a complete snprintf. */
int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    ULONG n;

    va_start(ap, fmt);
    n = ns_vformat(buf, (ULONG)size, fmt, ap);
    va_end(ap);
    return (int)n;
}
#endif /* DEBUG (netstack_diag_printf / snprintf) */

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

#define NSMEM_ORIGIN_EXEC 0x45584543UL /* 'EXEC' */
#define NSMEM_ORIGIN_DMA  0x444d4120UL /* 'DMA ' */
#define NSMEM_ORIGIN_SLB0 0x534C4230UL /* 'SLB0' — slab class 0 */
#define NSMEM_ORIGIN_SLB1 0x534C4231UL /* 'SLB1' */
#define NSMEM_ORIGIN_SLB2 0x534C4232UL /* 'SLB2' */

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
 * cleared. Note this frees arenas slightly earlier than the driver's own
 * pool teardown would — benign in the v1 quiesced shutdown (destroy ->
 * DETACH -> CloseDevice, no lwIP calls in between), where any block still
 * in flight is already lost either way; its later free takes the
 * leak-drop path below. v2 REATTACH hazard, flagged for the multi-netif
 * design phase: a stale slab-origin free after a second attach would
 * silently push foreign memory onto the new freelist (today's DMA analog
 * fails noisily inside dma_free); v1 attaches exactly once. */
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
    // KprintfH("[netstack] %s: size %lu\n", __func__, (ULONG)size);
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
    // KprintfH("[netstack] %s: count %lu size %lu\n", __func__, (ULONG)count, (ULONG)size);
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
    // KprintfH("[netstack] %s: ptr 0x%08lx\n", __func__, (ULONG)ptr);
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
