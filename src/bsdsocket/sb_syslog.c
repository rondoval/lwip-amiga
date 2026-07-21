/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * vsyslog — the library-side syslog sink. Honours the per-opener SBTC_LOG*
 * configuration (ident tag, priority mask) and routes the formatted message
 * to the debug backend.
 */

#include "sb_base.h"

#include <debug.h>

#include "netstack_diag.h"

VOID bsd_vsyslog(LONG pri asm("d0"), STRPTR msg asm("a0"), APTR args asm("a1"),
                 struct SocketBase *base asm("a6"))
{
    KprintfT("[bsdsocket] %s: pri=%ld fmt=%s\n", __func__, pri, (ULONG)msg);

    if (msg == NULL)
        return;
    /* drop priorities the opener masked off via SBTC_LOGMASK (setlogmask) */
    if (base != NULL && !(base->logMask & SB_LOG_MASK(pri)))
        return;

#ifdef DEBUG
    /* Below the debug tier the sink is a no-op, so don't format at all —
     * the LVO still exists and still honours the mask above. */
    {
        char buf[256];

        netstack_vformat_args(buf, sizeof(buf), (const char *)msg,
                              (const unsigned long *)args);
        if (base != NULL && base->logTagPtr != NULL)
            Kprintf("[syslog:%ld] %s: %s\n", pri, (ULONG)base->logTagPtr, (ULONG)buf);
        else
            Kprintf("[syslog:%ld] %s\n", pri, (ULONG)buf);
    }
#else
    (void)args; /* nothing formats it below the debug tier */
#endif
}
