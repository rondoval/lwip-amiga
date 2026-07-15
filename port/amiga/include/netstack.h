/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * netstack — the port-layer core: the one lock, time, memory, randomness.
 *
 * A single stack instance per system (bsdsocket.library is a singleton), so
 * the state is a library-global singleton; that is what lets the lwIP heap
 * hooks (which take no context) find their allocator.
 *
 * Locking discipline: EVERY call into lwIP — socket-layer entry, RX
 * injection, timer tick — is bracketed by netstack_lock()/netstack_unlock().
 * The lock is an Exec SignalSemaphore: task context only, never interrupts.
 */

#ifndef LWIPAMIGA_NETSTACK_H
#define LWIPAMIGA_NETSTACK_H

#include <exec/semaphores.h>
#include <exec/types.h>

struct Device;
struct NetdevIf;

struct NetStack
{
    struct SignalSemaphore ns_Core;

    /* time: EClock -> monotonic ms with divide-carry (no 64-bit division) */
    ULONG ns_EClockPerMs;
    ULONG ns_LastEClockLo;
    ULONG ns_TickRemainder;
    ULONG ns_Ms;

    ULONG ns_RandState;

    /* v1: the single attached NIC; routes the lwIP heap to its DMA
     * allocator (multi-netif TX pools are a design-phase open question) */
    struct NetdevIf *ns_ActiveNetdev;

    ULONG ns_MemInUse;  /* diagnostic */

    /* Core-lock profiling — written under DEBUG only, but the fields are
     * unconditional so DEBUG and release share one struct layout (the
     * 2026-07-14 Heisenbug lesson). Outermost holds only (ss_NestCount);
     * timestamps from the BCM 1 MHz system timer. netstack_tick prints
     * and zeroes the counters every ~2 s. */
    ULONG ns_LockT0;
    ULONG ns_LockHolds;
    ULONG ns_LockWaitUs;
    ULONG ns_LockHoldUs;
    ULONG ns_LockHoldMaxUs;
    ULONG ns_LockProfTicks;
};

/* The singleton (defined in netstack.c). */
extern struct NetStack netstack;

/* One-time init. @timerBase: an opened timer.device base (UNIT_ECLOCK or
 * UNIT_MICROHZ; only ReadEClock is used) owned by the caller and valid for
 * the stack's lifetime. Calls lwip_init() internally (under the lock). */
void netstack_init(struct Device *timerBase);

void netstack_lock(void);
void netstack_unlock(void);

/* Drive lwIP timeouts; call from the stack task every <= 100 ms. */
void netstack_tick(void);

/* Monotonic milliseconds (also lwIP's sys_now). Call under the lock. */
ULONG netstack_now_ms(void);

#endif /* LWIPAMIGA_NETSTACK_H */
