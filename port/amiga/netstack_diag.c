/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Diag formatting and the runtime log for the port layer. The formatting is
 * emu68-common's engine (format.h): C argument promotion, so lwIP's plain
 * 32-bit %d/%u/%x and %p render correctly, with no Exec call.
 *
 * The runtime log (netstack_log) exists at every tier: it feeds the log hook,
 * which is part of the library's API and must work in a release build. The debug
 * printers (netstack_diag_printf, snprintf) stay debug-tier:
 * netstack_diag_printf keeps its entry point (lwIP needs LWIP_PLATFORM_DIAG to
 * resolve) but its body compiles out, so a build with no sink is completely
 * silent. Hot paths never call any of this.
 */

#include "netstack_sys.h"

#include <stdarg.h>
#include <stddef.h> /* size_t, for the freestanding snprintf below */

#include <debug.h>
#include <format.h>

#include "netstack_diag.h"

#define NS_DIAG_BUF 256

/* --- the runtime log ------------------------------------------------------ */

/* The one registered sink (the library's log facility). The port layer has no
 * notion of hooks or openers: it formats and hands the line over. */
static netstack_log_sink_fn ns_log_sink;

void netstack_log_set_sink(netstack_log_sink_fn fn)
{
    ns_log_sink = fn;
}

void netstack_log(int pri, const char *fmt, ...)
{
    char buf[NS_DIAG_BUF];
    va_list ap;

    va_start(ap, fmt);
    _VSNPrintf((STRPTR)buf, sizeof(buf), (CONST_STRPTR)fmt, ap);
    va_end(ap);

    if (ns_log_sink != NULL)
        ns_log_sink(pri, buf);
    else
        Kprintf("[log:%ld] %s\n", (LONG)pri, (ULONG)buf); /* no sink: debug tiers only */
}

#ifdef DEBUG

/* LWIP_PLATFORM_DIAG (see lwipopts.h): streamed straight to the debug backend. */
void netstack_diag_printf(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    fmt_vformat(debug_putch, NULL, (CONST_STRPTR)fmt, ap);
    va_end(ap);
}

/* Freestanding snprintf. Two lwIP call sites need one at the debug tier: the
 * mDNS answer dump (mdns.c, compiled whenever LWIP_DEBUG is defined) and the
 * MEMP_OVERFLOW_CHECK assert text at TRACE. Pulling libnix's stdio to satisfy
 * them fails the link, because a -nostartfiles library binary carries no C
 * startup to supply SysBase/exit — and stdio drags malloc in behind it. It
 * covers the format subset of emu68-common's engine and returns C's value. */
int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    LONG n;

    va_start(ap, fmt);
    n = _VSNPrintf((STRPTR)buf, (ULONG)size, (CONST_STRPTR)fmt, ap);
    va_end(ap);
    return (int)n;
}

#else /* !DEBUG: keep the entry point, drop the output */

void netstack_diag_printf(const char *fmt, ...)
{
    (void)fmt;
}

#endif /* DEBUG */
