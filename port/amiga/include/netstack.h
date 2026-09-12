/* SPDX-License-Identifier: BSD-3-Clause */
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
#include <perf.h> /* struct lock_prof: core-lock wait/hold profiling */

struct Device;
struct NetIfBase;
struct NetdevIf;
struct Sana2If;

/* Slab front-end size classes over the packet heap (see netstack_mem.c). */
#define NS_SLAB_CLASSES 3

struct NetStack
{
    struct SignalSemaphore ns_Core;

    /* time: EClock -> monotonic ms with divide-carry (no 64-bit division) */
    ULONG ns_EClockPerMs;
    ULONG ns_LastEClockLo;
    ULONG ns_TickRemainder;
    ULONG ns_Ms;

    ULONG ns_RandState;

    /* The single attached hardware interface, seen two ways. ns_ActiveIf is
     * the backend-agnostic view (identity, VLAN, multicast set — what
     * sb_ifquery and the base hooks read); the typed pointers below are the
     * backend views, of which EXACTLY ONE is non-NULL and equal to
     * ns_ActiveIf whenever it is set. Heap routing and the outermost-lock
     * TX hooks key on the typed pointers (NULL = cheap no-op), so they stay
     * branch-light and cast-free. */
    struct NetIfBase *ns_ActiveIf;
    struct NetdevIf *ns_ActiveNetdev; /* also routes the lwIP heap to the
                                         driver's DMA allocator; NULL means
                                         the AllocMem fallback serves it */
    struct Sana2If *ns_ActiveSana2;

    ULONG ns_MemInUse;  /* diagnostic */

    /* packet-heap slab front-end (netstack_mem.c): O(1) per-class freelists,
     * all access under ns_Core, freelist links live inside the free slots.
     * TWO disjoint worlds that never share a freelist or an arena: the DMA
     * world (arenas from the attached netdev's allocator, returned at
     * detach) and the exec world (AllocMem arenas serving the no-netdev
     * case — SANA-II interfaces and the pre-attach window — persisting
     * until the stack task's final teardown). Which world serves an
     * allocation follows ns_ActiveNetdev; a block's origin word routes its
     * free back to the right world whenever it dies. */
    void *ns_SlabFree[NS_SLAB_CLASSES];    /* DMA-world freelist heads */
    void *ns_SlabArenas[NS_SLAB_CLASSES];  /* DMA-world NsSlabArena chains */
    ULONG ns_SlabGrows[NS_SLAB_CLASSES];   /* diagnostic */
    void *ns_SlabFreeX[NS_SLAB_CLASSES];   /* exec-world freelist heads */
    void *ns_SlabArenasX[NS_SLAB_CLASSES]; /* exec-world arena chains */
    ULONG ns_SlabGrowsX[NS_SLAB_CLASSES];  /* diagnostic */

    /* Core-lock profiling (emu68-common lock_prof): wait/hold timing of
     * ns_Core, outermost holds only. Written under PROFILE; the field is
     * unconditional so all tiers share one struct layout. netstack_tick
     * reports and rezeroes it every ~2 s via lock_prof_report(). */
    struct lock_prof ns_LockProf;
    ULONG ns_LockProfTicks;
};

/* The singleton (defined in netstack.c). */
extern struct NetStack netstack;

/* The stack task's timer.device base (defined in netstack.c, set by
 * netstack_init): ReadEClock for the ms clock, GetSysTime for timestamps. */
extern struct Device *TimerBase;

/* Init. @timerBase: an opened timer.device base (UNIT_ECLOCK or UNIT_MICROHZ;
 * only library calls are used) owned by the caller and valid for the stack's
 * lifetime. The first call runs lwip_init() (under the lock); later calls
 * (stack-task restart while the library stays loaded) only re-aim the time
 * base. */
void netstack_init(struct Device *timerBase);

void netstack_lock(void);
void netstack_unlock(void);

/* Drive lwIP timeouts; call from the stack task every <= 100 ms. */
void netstack_tick(void);

/* Monotonic milliseconds (also lwIP's sys_now). Call under the lock. */
ULONG netstack_now_ms(void);

/* Return the DMA-world slab arenas to @nd's DMA pool and reset that
 * world's freelists. netdevif_destroy calls this under the core lock,
 * before it clears ns_ActiveNetdev. The exec world is untouched. */
void netstack_slab_detach(struct NetdevIf *nd);

/* Free the exec-world arenas and forget their freelists. Only for the
 * stack task's final teardown — every interface down, every client gone,
 * nothing left that could hold a live slab block. A restarted stack task
 * regrows on demand; without this call the arenas would outlive the
 * library at expunge. */
void netstack_slab_exec_release(void);

#endif /* LWIPAMIGA_NETSTACK_H */
