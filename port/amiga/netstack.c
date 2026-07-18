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
 */

#define NSMEM_ORIGIN_EXEC 0x45584543UL /* 'EXEC' */
#define NSMEM_ORIGIN_DMA  0x444d4120UL /* 'DMA ' */

struct NsMemHeader
{
    ULONG nsm_Size; /* total, header included */
    ULONG nsm_Origin;
};

#if defined(DEBUG) && defined(DEBUG_HIGH)
/* Heap guards for the 2026-07-14 upload corruption hunt: a tail canary
 * catches overruns out of the block, poison-on-free makes use-after-free
 * loud (a poisoned origin also catches double-free), and DMA reads of
 * freed TX buffers put the pattern on the wire where a pcap can see it.
 * Guard hits report and freeze the caller — same policy as lwIP asserts.
 * DEBUG_HIGH tier only: the extra bytes and poison change heap layout and
 * demonstrably hide the corrupter (21:37 run passed, same speed without
 * guards wedged) — the no-HIGH debug build stays layout-faithful for
 * repro while the non-layout probes (pbuf gate, TCPGUARD) keep printing. */
#define NSMEM_TAIL_CANARY 0x4E534D54UL /* 'NSMT' */
#define NSMEM_POISON 0xDD

static void nsmem_dead(const char *what, struct NsMemHeader *h)
{
    Kprintf("[netstack] HEAP GUARD: %s block 0x%08lx (size %lu origin 0x%08lx) — task '%s' halted\n",
            (ULONG)what, (ULONG)h, h->nsm_Size, h->nsm_Origin,
            (ULONG)FindTask(NULL)->tc_Node.ln_Name);
    for (;;)
        Wait(0UL);
}
#endif

void *netstack_malloc(unsigned int size)
{
    // KprintfH("[netstack] %s: size %lu\n", __func__, (ULONG)size);
    ULONG asize = ((ULONG)size + 3) & ~3UL;
    ULONG total = asize + sizeof(struct NsMemHeader);
#if defined(DEBUG) && defined(DEBUG_HIGH)
    total += 4; /* tail canary */
#endif
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
#if defined(DEBUG) && defined(DEBUG_HIGH)
    *(ULONG *)((UBYTE *)(h + 1) + asize) = NSMEM_TAIL_CANARY;
#endif
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
#if defined(DEBUG) && defined(DEBUG_HIGH)
    if (h->nsm_Origin != NSMEM_ORIGIN_DMA && h->nsm_Origin != NSMEM_ORIGIN_EXEC)
        nsmem_dead("bad origin (double free / underrun / wild free)", h);
    if (*(ULONG *)((UBYTE *)h + h->nsm_Size - 4) != NSMEM_TAIL_CANARY)
        nsmem_dead("tail canary smashed (overrun)", h);
#endif
    netstack.ns_MemInUse -= h->nsm_Size;

    ULONG origin = h->nsm_Origin;
    ULONG size = h->nsm_Size;
#if defined(DEBUG) && defined(DEBUG_HIGH)
    UBYTE *b = (UBYTE *)h;
    for (ULONG i = 0; i < size; i++)
        b[i] = NSMEM_POISON;
#endif
    if (origin == NSMEM_ORIGIN_DMA)
        netdevif_dma_free(netstack.ns_ActiveNetdev, h, size);
    else
        FreeMem(h, size);
}
