/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * semstress — exec SignalSemaphore contended-handoff stress test.
 *
 * Repro tool for the 2026-07-13 whole-stack ~1 s freeze (notes.md, "Open
 * investigations"): with the pcap+log proving all lwip-amiga/genet code
 * lock-balanced, the remaining suspect is a lost wakeup in the exec
 * ObtainSemaphore/ReleaseSemaphore contended handoff as executed under the
 * Emu68 JIT. This tool recreates the exact task topology and timing shape
 * of the failure, with no networking involved:
 *
 *   "app"    pri 0: Obtain -> busy-hold ~10 ms (the WaitSelect scan window;
 *            the pri-5 tasks pile onto the semaphore here) -> arm 1 s timer
 *            -> SetSignal(own bit) -> Release -> immediately Wait().
 *            This release-then-Wait is the handoff under test.
 *   "unit"   pri 5: every ~2 ms, a burst of 3 Obtain/Release rounds
 *            (the genet RX bottom-half shape: the real freeze hit the
 *            Obtain immediately following a successful contended handoff).
 *   "tick"   pri 5: every ~25 ms one Obtain/Release (the stack-task tick;
 *            its wake arrives from a timer.device reply, so its Obtain
 *            races the handoff from interrupt-driven scheduling).
 *
 * Every Obtain is timed with ReadEClock. In a healthy system no acquisition
 * can take longer than the app's hold (~10 ms) plus scheduling noise; the
 * observed failure mode is 100 ms..1 s (bounded by the app's timer, which
 * is what shook the scheduler loose). Any "!!!" line = reproduced.
 *
 * The monitor task never touches the semaphore, so it keeps reporting even
 * if every role wedges: a role stuck >3 s in ST_OBTAIN with the semaphore
 * free is the smoking gun, stuck in ST_TIMER means a lost timer signal.
 *
 * Executive (dynamic scheduler) angle: if driver-style tasks sit inside
 * Executive's managed priority band they get demoted below a busy app task
 * that naps just often enough to look interactive — ready tasks then starve
 * for ~1 s until anti-starvation aging kicks in. To test exactly that:
 *   HOG 1  spawns a pri-0 spin-then-nap CPU burner (HOGBUSY ms busy,
 *          HOGIDLE ms nap — the nap keeps a decay scheduler boosting it);
 *   PRI n  sets the unit/tick static priority (5 = inside a "manage <=5"
 *          Executive band, 6+ = above it). Expected under Executive:
 *          PRI 5 + HOG 1 -> "!!!" stalls; PRI 6+ -> clean.
 * The monitor shows each role's live ln_Pri, so the demotion is visible.
 * Caveat observed on target: Executive buries a steady hog at the band
 * floor (p-47), so reproducing starvation likely needs phase alignment
 * (fresh-boosted spinner vs freshly-decayed victims) — not pursued further;
 * the production fix is static priorities above the managed band.
 *
 * Usage: semstress [RUNSECS n] [HOLDMS n] [APPMS n] [THRESH n]
 *                  [HAMMS n] [TICKMS n] [PRI n] [HOG 0/1]
 *                  [HOGBUSY n] [HOGIDLE n]
 *   defaults: RUNSECS 0 (until Ctrl-C), HOLDMS 10, APPMS 1000,
 *             THRESH 100, HAMMS 2, TICKMS 25, PRI 5, HOG 0,
 *             HOGBUSY 40, HOGIDLE 20
 * Exit code 0 = clean, 5 = over-threshold acquisitions seen.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <devices/timer.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <exec/io.h>
#include <exec/semaphores.h>
#include <exec/types.h>

#include <proto/exec.h>
#include <proto/dos.h>
#define TIMER_BASE_NAME TimerBase
#include <proto/timer.h>

#include <debug.h>

static const char verstring[] __attribute__((used)) = "$VER: semstress 1.0 (13.7.2026)";

struct Device *TimerBase;

/* role indices */
#define ROLE_APP  0
#define ROLE_UNIT 1
#define ROLE_TICK 2
#define ROLE_HOG  3
#define ROLE_N    4

/* what a role is doing right now — read by the monitor */
#define ST_INIT   0
#define ST_TIMER  1 /* in Wait() for its timer */
#define ST_OBTAIN 2 /* inside ObtainSemaphore */
#define ST_HOLD   3 /* owns the semaphore */
#define ST_EXIT   4

static const char *const stateName[] = { "init", "Wait(timer)", "ObtainSemaphore", "holding", "exited" };
static const char *const roleName[] = { "app", "unit", "tick", "hog" };

#define EV_RING 8

struct RoleStats
{
    volatile ULONG obtains;
    volatile ULONG overThresh;
    volatile ULONG maxLatMs;
    volatile ULONG heartbeat; /* bumped once per loop iteration */
    volatile UBYTE state;
    volatile UBYTE alive;
    struct Task *volatile task; /* live ln_Pri readout (Executive demotes) */
    /* over-threshold events, SPSC: role writes, monitor reads */
    volatile ULONG evLat[EV_RING];
    volatile ULONG evNum[EV_RING]; /* obtains counter at event time */
    volatile LONG evPri[EV_RING];  /* victim's ln_Pri at event time */
    volatile ULONG evProd;
};

struct SemStress
{
    struct SignalSemaphore sem;
    volatile ULONG stop;
    struct RoleStats role[ROLE_N];
    ULONG eclkPerMs;
    ULONG holdMs, appMs, hamMs, tickMs, threshMs;
    ULONG hogBusyMs, hogIdleMs;
    struct Task *parent;
};

/* spawn handshake: parent fills this, child copies it and signals CTRL_F */
static struct
{
    struct SemStress *ss;
    LONG role;
} gSpawn;

static ULONG now_lo(void)
{
    struct EClockVal ev;
    ReadEClock(&ev);
    return ev.ev_lo;
}

static void busy_spin_ticks(ULONG ticks)
{
    ULONG t0 = now_lo();
    while (now_lo() - t0 < ticks)
        ;
}

/* Timed ObtainSemaphore: the measurement at the heart of the tool. */
static ULONG measured_obtain(struct SemStress *ss, struct RoleStats *st)
{
    ULONG t0 = now_lo();
    st->state = ST_OBTAIN;
    ObtainSemaphore(&ss->sem);
    st->state = ST_HOLD;

    ULONG lat = (now_lo() - t0) / ss->eclkPerMs;
    st->obtains++;
    if (lat > st->maxLatMs)
        st->maxLatMs = lat;
    if (lat >= ss->threshMs)
    {
        ULONG i = st->evProd & (EV_RING - 1);
        st->evLat[i] = lat;
        st->evNum[i] = st->obtains;
        st->evPri[i] = st->task != NULL ? st->task->tc_Node.ln_Pri : 0;
        st->evProd++; /* after the payload: monitor reads prod first */
        st->overThresh++;
    }
    return lat;
}

/* One per-role timer; returns FALSE on Ctrl-C. extraSig participates in the
 * Wait mask (the app role's wake bit — matching WaitSelect exactly). */
struct RoleTimer
{
    struct MsgPort *port;
    struct timerequest *req;
};

static BOOL role_timer_wait(struct RoleTimer *rt, struct RoleStats *st, ULONG ms, ULONG extraSig)
{
    rt->req->tr_node.io_Command = TR_ADDREQUEST;
    rt->req->tr_time.tv_secs = ms / 1000;
    rt->req->tr_time.tv_micro = (ms % 1000) * 1000;
    SendIO(&rt->req->tr_node);

    st->state = ST_TIMER;
    ULONG sigs = Wait((1UL << rt->port->mp_SigBit) | extraSig | SIGBREAKF_CTRL_C);

    if (sigs & SIGBREAKF_CTRL_C)
    {
        AbortIO(&rt->req->tr_node);
        WaitIO(&rt->req->tr_node);
        return FALSE;
    }
    WaitIO(&rt->req->tr_node);
    return TRUE;
}

static BOOL role_timer_open(struct RoleTimer *rt)
{
    rt->port = CreateMsgPort();
    rt->req = (struct timerequest *)CreateIORequest(rt->port, sizeof(struct timerequest));
    if (rt->req == NULL ||
        OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_MICROHZ, &rt->req->tr_node, 0) != 0)
        return FALSE;
    return TRUE;
}

static void role_timer_close(struct RoleTimer *rt)
{
    if (rt->req != NULL && rt->req->tr_node.io_Device != NULL)
        CloseDevice(&rt->req->tr_node);
    if (rt->req != NULL)
        DeleteIORequest(&rt->req->tr_node);
    if (rt->port != NULL)
        DeleteMsgPort(rt->port);
}

/* ------------------------------------------------------------- the app --- */
/*
 * The failing WaitSelect shape, verbatim: obtain, do "work" while pri-5
 * tasks queue up behind the lock, arm the timer, clear the wake signal,
 * release (contended handoff), and Wait() with nothing else to do until
 * the timer fires. In the failure the release's wakeup was lost and the
 * whole system slept until this timer expired.
 */
static void run_app(struct SemStress *ss, struct RoleStats *st, struct RoleTimer *rt)
{
    BYTE wakeBit = AllocSignal(-1);
    ULONG wakeMask = wakeBit != -1 ? 1UL << wakeBit : 0;
    ULONG holdTicks = ss->holdMs * ss->eclkPerMs;

    while (!ss->stop)
    {
        measured_obtain(ss, st);
        busy_spin_ticks(holdTicks); /* the scan window: waiters pile up */

        /* WaitSelect order: arm timer under the lock, clear own signal,
         * release, immediately Wait */
        rt->req->tr_node.io_Command = TR_ADDREQUEST;
        rt->req->tr_time.tv_secs = ss->appMs / 1000;
        rt->req->tr_time.tv_micro = (ss->appMs % 1000) * 1000;
        SendIO(&rt->req->tr_node);
        SetSignal(0UL, wakeMask);
        st->state = ST_TIMER;
        ReleaseSemaphore(&ss->sem);

        ULONG sigs = Wait((1UL << rt->port->mp_SigBit) | wakeMask | SIGBREAKF_CTRL_C);
        if (sigs & SIGBREAKF_CTRL_C)
        {
            AbortIO(&rt->req->tr_node);
            WaitIO(&rt->req->tr_node);
            break;
        }
        WaitIO(&rt->req->tr_node);
        st->heartbeat++;
    }

    if (wakeBit != -1)
        FreeSignal(wakeBit);
}

/* ------------------------------------------- the pri-5 hammer variants --- */
/*
 * unit: short period, burst of rounds back-to-back — the real freeze hit
 * the Obtain right after a successful contended handoff, which a burst
 * reproduces (round 1 queues during the app's hold, rounds 2-3 follow
 * within microseconds of the handoff).
 * tick: one round per longer period, woken by a timer.device reply.
 */
static void run_hammer(struct SemStress *ss, struct RoleStats *st, struct RoleTimer *rt,
                       ULONG periodMs, ULONG burst, ULONG subHoldUs)
{
    ULONG subTicks = (ss->eclkPerMs * subHoldUs) / 1000;

    while (!ss->stop)
    {
        if (!role_timer_wait(rt, st, periodMs, 0))
            break;

        for (ULONG r = 0; r < burst && !ss->stop; r++)
        {
            measured_obtain(ss, st);
            busy_spin_ticks(subTicks);
            ReleaseSemaphore(&ss->sem);
        }
        st->heartbeat++;
    }
}

/* ----------------------------------------------------------- the hog --- */
/*
 * The Executive trap: burn CPU but nap every HOGIDLE ms, so a decay-based
 * scheduler keeps scoring it "interactive" while it eats most of the CPU.
 * Never touches the semaphore — starvation of the other roles is pure
 * scheduling. Mimics an app main task busy-polling its worker.
 */
static void run_hog(struct SemStress *ss, struct RoleStats *st, struct RoleTimer *rt)
{
    ULONG busyTicks = ss->hogBusyMs * ss->eclkPerMs;

    while (!ss->stop)
    {
        st->state = ST_HOLD; /* "busy" */
        busy_spin_ticks(busyTicks);
        if (!role_timer_wait(rt, st, ss->hogIdleMs, 0))
            break;
        st->heartbeat++;
    }
}

/* --------------------------------------------------------- child frame --- */

static void RoleTask(void)
{
    struct SemStress *ss = gSpawn.ss;
    LONG role = gSpawn.role;
    struct RoleStats *st = &ss->role[role];

    st->task = FindTask(NULL);
    st->alive = 1;
    Signal(ss->parent, SIGBREAKF_CTRL_F);

    struct RoleTimer rt;
    rt.port = NULL;
    rt.req = NULL;
    if (role_timer_open(&rt))
    {
        switch (role)
        {
        case ROLE_APP:
            run_app(ss, st, &rt);
            break;
        case ROLE_UNIT:
            run_hammer(ss, st, &rt, ss->hamMs, 3, 200);
            break;
        case ROLE_TICK:
            run_hammer(ss, st, &rt, ss->tickMs, 1, 100);
            break;
        case ROLE_HOG:
            run_hog(ss, st, &rt);
            break;
        }
    }
    role_timer_close(&rt);

    st->state = ST_EXIT;
    st->alive = 0;
    Signal(ss->parent, SIGBREAKF_CTRL_F);
}

static BOOL spawn_role(struct SemStress *ss, LONG role, LONG pri)
{
    gSpawn.ss = ss;
    gSpawn.role = role;
    SetSignal(0UL, SIGBREAKF_CTRL_F);

    struct Process *p = CreateNewProcTags(
        NP_Entry, (ULONG)RoleTask,
        NP_Name, (ULONG)roleName[role],
        NP_Priority, pri,
        NP_StackSize, 8192,
        TAG_DONE);
    if (p == NULL)
        return FALSE;

    Wait(SIGBREAKF_CTRL_F);
    return TRUE;
}

/* ------------------------------------------------------------- monitor --- */

static void report_events(struct SemStress *ss, ULONG *seen)
{
    for (LONG i = 0; i < ROLE_N; i++)
    {
        struct RoleStats *st = &ss->role[i];
        ULONG prod = st->evProd;
        /* on overflow of the tiny ring just show the newest EV_RING */
        if (prod - seen[i] > EV_RING)
            seen[i] = prod - EV_RING;
        while (seen[i] != prod)
        {
            ULONG k = seen[i] & (EV_RING - 1);
            printf("!!! %s: ObtainSemaphore took %lu ms (obtain #%lu, pri %ld)\n",
                   roleName[i], (unsigned long)st->evLat[k], (unsigned long)st->evNum[k],
                   (long)st->evPri[k]);
            Kprintf("[semstress] !!! %s: obtain %lu ms (#%lu, pri %ld)\n",
                    (ULONG)roleName[i], st->evLat[k], st->evNum[k], (ULONG)st->evPri[k]);
            seen[i]++;
        }
    }
}

static BOOL strieq(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0')
    {
        char ca = *a++, cb = *b++;
        if (ca >= 'a' && ca <= 'z')
            ca -= 32;
        if (cb >= 'a' && cb <= 'z')
            cb -= 32;
        if (ca != cb)
            return FALSE;
    }
    return *a == *b;
}

static ULONG parse_arg(int argc, char **argv, const char *key, ULONG def)
{
    for (int i = 1; i + 1 < argc; i++)
    {
        if (strieq(argv[i], key))
            return (ULONG)atoi(argv[i + 1]);
    }
    return def;
}

int main(int argc, char **argv)
{
    int rc = 20;

    struct SemStress *ss = AllocMem(sizeof(*ss), MEMF_PUBLIC | MEMF_CLEAR);
    if (ss == NULL)
    {
        printf("semstress: out of memory\n");
        return rc;
    }
    InitSemaphore(&ss->sem);
    ss->parent = FindTask(NULL);

    ULONG runSecs = parse_arg(argc, argv, "RUNSECS", 0);
    ss->holdMs = parse_arg(argc, argv, "HOLDMS", 10);
    ss->appMs = parse_arg(argc, argv, "APPMS", 1000);
    ss->hamMs = parse_arg(argc, argv, "HAMMS", 2);
    ss->tickMs = parse_arg(argc, argv, "TICKMS", 25);
    ss->threshMs = parse_arg(argc, argv, "THRESH", 100);
    LONG rolePri = (LONG)parse_arg(argc, argv, "PRI", 5);
    ULONG hogOn = parse_arg(argc, argv, "HOG", 0);
    ss->hogBusyMs = parse_arg(argc, argv, "HOGBUSY", 40);
    ss->hogIdleMs = parse_arg(argc, argv, "HOGIDLE", 20);

    /* monitor timer doubles as the EClock source for everyone */
    struct RoleTimer mt;
    mt.port = NULL;
    mt.req = NULL;
    if (!role_timer_open(&mt))
    {
        printf("semstress: no timer.device\n");
        goto out;
    }
    TimerBase = mt.req->tr_node.io_Device;

    struct EClockVal ev;
    ULONG freq = ReadEClock(&ev);
    ss->eclkPerMs = freq / 1000;
    if (ss->eclkPerMs == 0)
        ss->eclkPerMs = 1;

    printf("semstress: eclock %lu Hz; hold %lu ms, app period %lu ms, "
           "unit %lu ms x3, tick %lu ms, threshold %lu ms, pri %ld%s%s\n",
           (unsigned long)freq, (unsigned long)ss->holdMs, (unsigned long)ss->appMs,
           (unsigned long)ss->hamMs, (unsigned long)ss->tickMs, (unsigned long)ss->threshMs,
           (long)rolePri, hogOn ? ", HOG on" : "",
           runSecs ? "" : ", Ctrl-C to stop");
    printf("semstress: healthy max latency = hold+noise; any '!!!' = reproduced\n");
    Kprintf("[semstress] start: hold %lu app %lu ham %lu tick %lu thresh %lu pri %ld hog %lu\n",
            ss->holdMs, ss->appMs, ss->hamMs, ss->tickMs, ss->threshMs, (ULONG)rolePri, hogOn);

    ULONG seen[ROLE_N] = { 0, 0, 0 };
    ULONG prevBeat[ROLE_N] = { 0, 0, 0 };
    ULONG stalledSecs[ROLE_N] = { 0, 0, 0 };
    ULONG secs = 0;
    BOOL stopped = FALSE;

    if (!spawn_role(ss, ROLE_UNIT, rolePri) ||
        !spawn_role(ss, ROLE_TICK, rolePri) ||
        !spawn_role(ss, ROLE_APP, 0) ||
        (hogOn && !spawn_role(ss, ROLE_HOG, 0)))
    {
        printf("semstress: task spawn failed\n");
        ss->stop = 1;
        goto out_children;
    }

    /* monitor loop: 1 s cadence, drains event rings, watches for wedged
     * roles (heartbeat stalled). Never touches ss->sem. */

    while (!stopped)
    {
        mt.req->tr_node.io_Command = TR_ADDREQUEST;
        mt.req->tr_time.tv_secs = 1;
        mt.req->tr_time.tv_micro = 0;
        SendIO(&mt.req->tr_node);
        ULONG sigs = Wait((1UL << mt.port->mp_SigBit) | SIGBREAKF_CTRL_C);
        if (sigs & SIGBREAKF_CTRL_C)
        {
            AbortIO(&mt.req->tr_node);
            WaitIO(&mt.req->tr_node);
            stopped = TRUE;
        }
        else
            WaitIO(&mt.req->tr_node);

        secs++;
        report_events(ss, seen);

        for (LONG i = 0; i < ROLE_N && !stopped; i++)
        {
            struct RoleStats *st = &ss->role[i];
            if (st->task == NULL)
                continue; /* never spawned (HOG off) */
            ULONG beat = st->heartbeat;
            stalledSecs[i] = (beat == prevBeat[i]) ? stalledSecs[i] + 1 : 0;
            prevBeat[i] = beat;
            if (st->alive && stalledSecs[i] >= 3)
                printf("!!! %s wedged for %lus in state '%s' (obtains=%lu, pri %ld)\n",
                       roleName[i], (unsigned long)stalledSecs[i],
                       stateName[st->state], (unsigned long)st->obtains,
                       (long)st->task->tc_Node.ln_Pri);
        }

        if (secs % 10 == 0 || (runSecs && secs >= runSecs))
        {
            /* live ln_Pri makes an Executive-style demotion visible */
            printf("[%4lus]", (unsigned long)secs);
            for (LONG i = 0; i < ROLE_N; i++)
            {
                struct RoleStats *st = &ss->role[i];
                if (st->task == NULL)
                    continue;
                LONG pri = st->alive ? st->task->tc_Node.ln_Pri : 0;
                if (i == ROLE_HOG)
                    printf(" | hog p%ld", (long)pri);
                else
                    printf("%s %s p%ld: %lu obt max %lu over %lu",
                           i > 0 ? " |" : "", roleName[i], (long)pri,
                           (unsigned long)st->obtains,
                           (unsigned long)st->maxLatMs,
                           (unsigned long)st->overThresh);
            }
            printf("\n");
        }

        if (runSecs && secs >= runSecs)
            stopped = TRUE;
    }

out_children:
    ss->stop = 1;
    for (LONG i = 0; i < ROLE_N; i++)
    {
        struct RoleStats *st = &ss->role[i];
        /* alive-check + Signal under one Forbid: the child clears alive
         * before it can exit, so the task pointer stays valid here */
        Forbid();
        if (st->alive && st->task != NULL)
            Signal(st->task, SIGBREAKF_CTRL_C);
        Permit();
    }
    for (LONG waited = 0;; waited++)
    {
        BOOL any = FALSE;
        for (LONG i = 0; i < ROLE_N; i++)
            if (ss->role[i].alive)
                any = TRUE;
        if (!any)
            break;
        Delay(10); /* 200 ms */
        if (waited == 25)
            printf("semstress: waiting for tasks to exit "
                   "(a wedge here IS the repro; reboot if it never returns)\n");
    }

    report_events(ss, seen); /* anything that fired between the last tick and shutdown */

    ULONG totalOver = 0;
    printf("--- summary ---\n");
    for (LONG i = 0; i < ROLE_HOG; i++)
    {
        printf("%s: %lu obtains, max latency %lu ms, over-threshold %lu\n",
               roleName[i], (unsigned long)ss->role[i].obtains,
               (unsigned long)ss->role[i].maxLatMs, (unsigned long)ss->role[i].overThresh);
        totalOver += ss->role[i].overThresh;
    }
    if (totalOver != 0)
    {
        printf("REPRODUCED: %lu acquisition(s) over %lu ms\n",
               (unsigned long)totalOver, (unsigned long)ss->threshMs);
        rc = 5;
    }
    else
    {
        printf("no anomalies\n");
        rc = 0;
    }

    role_timer_close(&mt);
out:
    FreeMem(ss, sizeof(*ss));
    return rc;
}
