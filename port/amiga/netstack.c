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

#include "netstack_diag.h" /* netstack_diag_printf: %p / 32-bit %u guard dumps */

#include <lwip/init.h>
#include <lwip/netif.h>
#include <lwip/sys.h>
#include <lwip/timeouts.h>

#include "netstack.h"
#include "netdev_if.h" /* netdevif_tx_kick / netdevif_tx_reclaim at outermost lock */
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
    lock_prof_init(&netstack.ns_LockProf, "netstack");

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
    lock_prof_obtain(&netstack.ns_LockProf, &netstack.ns_Core);
    /* Reap completed TX at the outermost obtain, before any tcp_output in this
     * hold: the frees fold into a hold that already exists (no dedicated reclaim
     * lock on the unit task) and the transmitted frames' refs drop before RX
     * fast-retransmit or tcp_write re-examine them. Symmetric with the
     * netdevif_tx_kick publish at the outermost unlock. */
    if (netstack.ns_Core.ss_NestCount == 1)
        netdevif_tx_reclaim(netstack.ns_ActiveNetdev);
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
        ULONG rounds = 0;
        while (netstack_loopback_pending())
        {
            if (++rounds > 1000)
            {
                /* Unbounded drain in the unlocking task's context: a stuck
                 * or cyclic loop queue would spin here forever holding
                 * ns_Core. Confess and leak the queue instead. */
#ifdef DEBUG
                struct netif *nif;
                netstack_diag_printf("[netstack] LOOP-PUMP-GUARD: drain stuck, task '%s'\n",
                                     FindTask(NULL)->tc_Node.ln_Name);
                NETIF_FOREACH(nif)
                {
                    struct pbuf *q = nif->loop_first;
                    netstack_diag_printf("  netif %c%c%d loop_first=%p len=%u tot=%u next=%p last=%p\n",
                                         nif->name[0], nif->name[1], nif->num,
                                         (void *)q, q != NULL ? q->len : 0,
                                         q != NULL ? q->tot_len : 0,
                                         (void *)(q != NULL ? q->next : NULL),
                                         (void *)nif->loop_last);
                }
#endif
                break;
            }
            netif_poll_all();
        }

        /* Publish any TX batch staged during this hold with a single doorbell.
         * After the loopback drain so loopback-generated TX is included; still
         * under the lock, so it cannot race the driver's own datapath. */
        netdevif_tx_kick(netstack.ns_ActiveNetdev);
    }
    lock_prof_release(&netstack.ns_LockProf, &netstack.ns_Core);
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
#ifdef PROFILE
    /* report every ~2 s (20 × 100 ms ticks): the core-lock wait/hold slots
     * then the per-stage timings, both through perf_report */
    if (++netstack.ns_LockProfTicks >= 20)
    {
        netstack.ns_LockProfTicks = 0;
        lock_prof_report(&netstack.ns_LockProf);
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
 * corruption.
 *
 * Reached only at the debug tier and above. Below it lwipopts.h sets
 * LWIP_NOASSERT, so every LWIP_ASSERT compiles away and nothing calls this at
 * all. lwIP's LWIP_ERROR guards are a separate mechanism and are unaffected:
 * they route through LWIP_PLATFORM_DIAG and always run their handler (typically
 * `return ERR_ARG`), so argument validation still recovers in every build. */
void netstack_platform_diag(const char *msg)
{
    (void)msg; /* the only reader is Kprintf, which compiles out below debug */
    Kprintf("[lwip] ASSERT: %s — task '%s' halted\n", (ULONG)msg,
            (ULONG)FindTask(NULL)->tc_Node.ln_Name);
    for (;;)
        Wait(0UL);
}
