/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * netstack core: the one lock, time, randomness and the lwIP platform
 * glue. The packet heap lives in netstack_mem.c, the DEBUG diag formatter
 * in netstack_diag.c.
 */

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
    /* lock wait/hold profiling; outermost holds only */
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
#ifdef DEBUG
    if (netstack.ns_Core.ss_Owner != FindTask(NULL))
        Kprintf("[netstack] core lock NOT held by caller!\n");
#endif
}

void netstack_tick(void)
{
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

/* LWIP_PLATFORM_ASSERT. lwIP's LWIP_ASSERT assumes this never returns —
 * returning would let the core continue into state it has declared
 * impossible, corrupting memory at some later, unrelated point. Freezing
 * the calling task keeps the failed state intact for post-mortem; other
 * tasks then park on ns_Core, which is a deterministic stall instead of
 * corruption. The message is latched for release builds, where Kprintf
 * compiles out. */
const char *netstack_assert_msg;

void netstack_platform_diag(const char *msg)
{
    netstack_assert_msg = msg;
    Kprintf("[lwip] ASSERT: %s — task '%s' halted\n", (ULONG)msg,
            (ULONG)FindTask(NULL)->tc_Node.ln_Name);
    for (;;)
        Wait(0UL);
}
