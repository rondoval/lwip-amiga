/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The runtime log facility — see sb_log.h for the contract. Three inputs
 * converge here, all below: bsd_vsyslog (client lines, the vsyslog LVO),
 * netstack_log through the sink registered at init (the stack's own lines,
 * from both the library and the port layer), and the lwIP netif observer
 * (link and address events, which no emitter site has to remember to report).
 */

#include "sb_base.h"

#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#include <clib/timer_protos.h>
#else
#include <proto/exec.h>
#define TIMER_BASE_NAME TimerBase
#include <proto/timer.h>
#endif

#include <exec/memory.h>

#include <debug.h>

#include <lwip/dhcp.h>
#include <lwip/ip4_addr.h>
#include <lwip/netif.h>

#include <netif_base.h>

#include "netstack.h"

_Static_assert(sizeof(struct SbLogHookMessage) == 32, "LogHookMessage ABI");

/* The library is a singleton; the netstack sink has no context argument. */
static struct SocketBase *sbLogRoot;

/* --- delivery ------------------------------------------------------------- */

/* Hook call, Amiga convention: hook A0, object A2 (reserved, NULL), message
 * A1, entry at h_Entry. Same construct as the toolchain's inline library
 * stubs (register variables around an inline jsr); the scratch registers are
 * outputs so the compiler expects them clobbered, a3 is our scratch. */
static void sb_log_call(struct Hook *hook, struct SbLogHookMessage *lhm)
{
    register struct Hook *rA0 __asm("a0") = hook;
    register struct SbLogHookMessage *rA1 __asm("a1") = lhm;
    register APTR rA2 __asm("a2") = NULL;
    register int rD0 __asm("d0");
    register int rD1 __asm("d1");

    __asm volatile("movea.l a0@(8),a3\n\tjsr a3@"
                   : "=r"(rD0), "=r"(rD1), "+r"(rA0), "+r"(rA1), "+r"(rA2)
                   :
                   : "a3", "memory", "cc");
}

/* Amiga epoch seconds -> DateStamp; zero when the stack has no timer yet
 * (a line nobody can receive anyway). No dos.library: DateStamp() needs a
 * Process, and emitters include the driver's bare unit task. */
static void sb_log_stamp(struct DateStamp *ds)
{
    if (TimerBase == NULL)
    {
        ds->ds_Days = 0;
        ds->ds_Minute = 0;
        ds->ds_Tick = 0;
        return;
    }
    struct timeval tv;
    GetSysTime(&tv);
    ds->ds_Days = (LONG)(tv.tv_secs / 86400UL);
    ULONG rem = tv.tv_secs % 86400UL;
    ds->ds_Minute = (LONG)(rem / 60UL);
    ds->ds_Tick = (LONG)((rem % 60UL) * TICKS_PER_SECOND +
                         tv.tv_micro / (1000000UL / TICKS_PER_SECOND));
}

static void sb_log_ring_put(struct SocketBase *root, const struct SbLogHookMessage *lhm)
{
    if (root->logRing == NULL)
        return;
    struct SbLogEntry *e = &root->logRing[root->logRingHead];
    root->logRingHead = (UBYTE)((root->logRingHead + 1) % SB_LOG_RING);
    if (root->logRingCount < SB_LOG_RING)
        root->logRingCount++;
    e->le_Date = lhm->lhm_Date;
    e->le_Id = lhm->lhm_ID;
    e->le_Pri = (UBYTE)lhm->lhm_Priority;
    strlcpy(e->le_Tag, lhm->lhm_Tag != NULL ? (const char *)lhm->lhm_Tag : "",
            sizeof(e->le_Tag));
    strlcpy(e->le_Text, (const char *)lhm->lhm_Message, sizeof(e->le_Text));
}

/* Under Forbid with logBusy set by the caller: hand the ring, oldest first,
 * to the hook just installed, then forget it. */
static void sb_log_ring_replay(struct SocketBase *root, struct Hook *hook)
{
    ULONG idx = (root->logRingHead + SB_LOG_RING - root->logRingCount) % SB_LOG_RING;
    for (ULONG n = root->logRingCount; n > 0; n--)
    {
        struct SbLogEntry *e = &root->logRing[idx];
        struct SbLogHookMessage lhm;
        lhm.lhm_Size = sizeof(lhm);
        lhm.lhm_Priority = e->le_Pri;
        lhm.lhm_Date = e->le_Date;
        lhm.lhm_Tag = e->le_Tag[0] != '\0' ? (STRPTR)e->le_Tag : NULL;
        lhm.lhm_ID = e->le_Id;
        lhm.lhm_Message = (STRPTR)e->le_Text;
        sb_log_call(hook, &lhm);
        idx = (idx + 1) % SB_LOG_RING;
    }
    root->logRingCount = 0;
    root->logRingHead = 0;
}

void sb_log_emit(struct SocketBase *root, LONG pri, STRPTR tag, ULONG id, char *text)
{
    /* syslog() callers may end the line for a console sink; the hook and the
     * ring want the bare line */
    ULONG len = strlen(text);
    if (len > 0 && text[len - 1] == '\n')
        text[len - 1] = '\0';

    struct SbLogHookMessage lhm;
    lhm.lhm_Size = sizeof(lhm);
    lhm.lhm_Priority = pri & SB_LOG_PRIMASK;
    sb_log_stamp(&lhm.lhm_Date);
    lhm.lhm_Tag = tag;
    lhm.lhm_ID = id;
    lhm.lhm_Message = (STRPTR)text;

    Forbid();
    if (!root->logBusy)
    {
        root->logBusy = TRUE;
        if (root->logHook != NULL)
            sb_log_call(root->logHook, &lhm);
        else
            sb_log_ring_put(root, &lhm);
        root->logBusy = FALSE;
    }
    Permit();

    /* the debug backend mirrors every line regardless, so a serial capture
     * of a hardware session stays complete while a viewer runs */
    Kprintf("[log:%ld] %s: %s\n", lhm.lhm_Priority,
            (ULONG)(tag != NULL ? tag : (STRPTR)"-"), (ULONG)text);
}

/* --- hook management ------------------------------------------------------ */

void sb_log_set_hook(struct SocketBase *root, struct Hook *hook, struct SocketBase *owner)
{
    Kprintf("[bsdsocket] %s: hook 0x%08lx owner 0x%08lx\n", __func__, (ULONG)hook, (ULONG)owner);
    Forbid();
    root->logHook = hook;
    root->logHookOwner = hook != NULL ? owner : NULL;
    if (hook != NULL && root->logRing != NULL && root->logRingCount != 0 && !root->logBusy)
    {
        root->logBusy = TRUE;
        sb_log_ring_replay(root, hook);
        root->logBusy = FALSE;
    }
    Permit();
}

void sb_log_owner_closed(struct SocketBase *root, struct SocketBase *owner)
{
    Forbid();
    if (root->logHookOwner == owner)
    {
        Kprintf("[bsdsocket] %s: retracting the log hook of base 0x%08lx\n", __func__, (ULONG)owner);
        root->logHook = NULL;
        root->logHookOwner = NULL;
    }
    Permit();
}

/* --- the vsyslog LVO (client lines) --------------------------------------- */

/* The client side of the log: honours the opener's SBTC_LOG* configuration
 * (ident tag, default facility, priority mask), expands %m — BSD syslog(3)'s
 * "the text of errno", which only the library can supply since that errno is
 * the opener's — formats with the component's one formatter and hands the
 * line to sb_log_emit above.
 *
 * This is an LVO: it runs on the calling program's stack. */

/* "%m" anywhere in fmt, ignoring "%%" escapes */
static BOOL sb_syslog_has_m(const char *fmt)
{
    for (; *fmt != '\0'; fmt++)
    {
        if (*fmt != '%')
            continue;
        if (fmt[1] == 'm')
            return TRUE;
        if (fmt[1] == '%')
            fmt++;
    }
    return FALSE;
}

/* fmt -> dst with every %m replaced by errText; "%%" passes through so a
 * literal "%%m" stays literal for the formatter */
static void sb_syslog_expand_m(char *dst, ULONG max, const char *fmt, const char *errText)
{
    ULONG o = 0;
    while (*fmt != '\0' && o < max - 1)
    {
        if (fmt[0] == '%' && fmt[1] == 'm')
        {
            for (const char *e = errText; *e != '\0' && o < max - 1; e++)
                dst[o++] = *e;
            fmt += 2;
            continue;
        }
        if (fmt[0] == '%' && fmt[1] == '%' && o + 1 < max - 1)
        {
            dst[o++] = *fmt++;
        }
        dst[o++] = *fmt++;
    }
    dst[o] = '\0';
}

VOID bsd_vsyslog(LONG pri asm("d0"), STRPTR msg asm("a0"), APTR args asm("a1"),
                 struct SocketBase *base asm("a6"))
{
    KprintfT("[bsdsocket] %s: pri=%ld fmt=%s\n", __func__, pri, (ULONG)msg);

    if (msg == NULL)
        return;
    /* drop levels the opener masked off via SBTC_LOGMASK (setlogmask) */
    if (!(base->logMask & SB_LOG_MASK(pri)))
        return;

    const char *fmt = (const char *)msg;
    if (sb_syslog_has_m(fmt))
    {
        sb_syslog_expand_m(base->logFmtBuf, sizeof(base->logFmtBuf), fmt,
                           sb_errno_text(base->internalErrno));
        fmt = base->logFmtBuf;
    }

    char buf[SB_SYSLOG_BUF];
    netstack_vformat_args(buf, sizeof(buf), fmt, (const unsigned long *)args);

    /* a priority word without facility bits takes the opener's default */
    ULONG facility = (ULONG)pri & SB_LOG_FACMASK;
    if (facility == 0)
        facility = base->logFacility;
    sb_log_emit(SB_ROOT(base), pri & SB_LOG_PRIMASK, base->logTagPtr, facility, buf);
}

/* --- the netstack sink ---------------------------------------------------- */

static void sb_log_sink(int pri, char *text)
{
    sb_log_emit(sbLogRoot, (LONG)pri, (STRPTR)SB_LOG_TAG, 0, text);
}

void sb_log_init(struct SocketBase *root)
{
    root->logHook = NULL;
    root->logHookOwner = NULL;
    root->logBusy = FALSE;
    root->logRingHead = 0;
    root->logRingCount = 0;
    /* separate allocation: the child bases are byte copies of the root, so
     * anything embedded here would be duplicated per opener */
    root->logRing = AllocMem(sizeof(struct SbLogEntry) * SB_LOG_RING, MEMF_PUBLIC | MEMF_CLEAR);
    sbLogRoot = root;
    netstack_log_set_sink(sb_log_sink);
}

void sb_log_exit(struct SocketBase *root)
{
    netstack_log_set_sink(NULL);
    sbLogRoot = NULL;
    if (root->logRing != NULL)
    {
        FreeMem(root->logRing, sizeof(struct SbLogEntry) * SB_LOG_RING);
        root->logRing = NULL;
    }
}

/* --- the lwIP netif observer ---------------------------------------------- */

/* Runs under the core lock on whichever task drove the change: the stack
 * task (DHCP timers, static config) or the driver's unit task (link). */
static void sb_log_netif_cb(struct netif *nif, netif_nsc_reason_t reason,
                            const netif_ext_callback_args_t *args)
{
    if (sb_if_is_loopback(nif))
        return;
    const char *name = ((struct NetIfBase *)nif->state)->nib_Name;

    if (reason & LWIP_NSC_LINK_CHANGED)
        SB_LOG(NS_LOG_NOTICE, "%s: link %s", name, args->link_changed.state ? "up" : "down");

    /* ADDRESS_CHANGED fires only for a real change (bind, static config,
     * lease lost, rebind to a different address); a renewal that keeps the
     * address raises just ADDR_VALID and is not worth a line */
    if (reason & LWIP_NSC_IPV4_ADDRESS_CHANGED)
    {
        char addr[IP4ADDR_STRLEN_MAX];
        if (ip4_addr_isany(netif_ip4_addr(nif)))
        {
            ip4addr_ntoa_r(ip_2_ip4(args->ipv4_changed.old_address), addr, sizeof(addr));
            SB_LOG(NS_LOG_NOTICE, "%s: address %s released", name, addr);
        }
        else
        {
            char mask[IP4ADDR_STRLEN_MAX];
            char gw[IP4ADDR_STRLEN_MAX];
            ip4addr_ntoa_r(netif_ip4_addr(nif), addr, sizeof(addr));
            ip4addr_ntoa_r(netif_ip4_netmask(nif), mask, sizeof(mask));
            ip4addr_ntoa_r(netif_ip4_gw(nif), gw, sizeof(gw));
            SB_LOG(NS_LOG_NOTICE, "%s: address %s netmask %s gateway %s (%s)", name, addr,
                   mask, gw, dhcp_supplied_address(nif) ? "DHCP lease" : "static");
        }
    }
}

static netif_ext_callback_t sbLogNetifCb;
static BOOL sbLogNetifAttached;

void sb_log_netif_attach(void)
{
    /* netstack_init is one-shot across stack-task restarts; so is the
     * subscription (lwIP keeps the callback node linked) */
    if (sbLogNetifAttached)
        return;
    netstack_lock();
    netif_add_ext_callback(&sbLogNetifCb, sb_log_netif_cb);
    netstack_unlock();
    sbLogNetifAttached = TRUE;
}
