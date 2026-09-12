/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Diag formatting and the runtime log for the port layer (implemented in
 * netstack_diag.c).
 *
 * Every entry point formats with C argument promotion — 32-bit %d/%u/%x, %p —
 * which RawDoFmt (behind Kprintf) cannot do: it reads bare %d/%u/%x as 16-bit
 * and has no %p. Only the finished string reaches the sink. The format rules
 * are therefore C's, with one formatter's limits: every argument cell is
 * 32-bit, so %ld/%lu/%lx equal %d/%u/%x (the l/h/z modifiers are skipped);
 * %p prints 8 hex digits; %s of NULL prints "(null)"; width, precision, '-'
 * and '0' are honoured; there is no %m, no 64-bit and no floating point.
 *
 * Deliberately dependency-free, and plain C types rather than the usual
 * ULONG: lwipopts.h includes this, and lwipopts.h is pulled in by lwip/opt.h
 * ahead of both lwIP's and Exec's type layers — dragging <exec/types.h> into
 * every lwIP translation unit is a risk with nothing to gain. Do not add
 * includes here.
 */

#ifndef LWIPAMIGA_NETSTACK_DIAG_H
#define LWIPAMIGA_NETSTACK_DIAG_H

/* LWIP_PLATFORM_DIAG (see lwipopts.h). Body is debug-tier; below that this is
 * a no-op, so lwIP's own diag and the fork's evidence printers go silent while
 * their checks keep running. */
void netstack_diag_printf(const char *fmt, ...);

/* Same formatter, arguments taken from a flat 32-bit array instead of varargs
 * — the AmigaOS vsyslog calling convention. Backs bsd_vsyslog (a public LVO),
 * which is why this is exported rather than static to netstack_diag.c.
 * Writes at most max-1 chars plus a NUL; returns the number written. */
unsigned long netstack_vformat_args(char *dst, unsigned long max,
                                    const char *fmt, const unsigned long *args);

/*
 * The runtime log: operational events — interface and link state, lease
 * changes, failures — that must reach an operator in a release build. It is
 * NOT a trace facility: never call it per packet, per call or per tick; the
 * tier-gated Kprintf family stays the tool for that.
 *
 * Priorities are <sys/syslog.h>'s LOG_* values; a line is formatted into a
 * stack buffer and handed to the one registered sink (the library's log
 * facility), which owns the delivery — log hook, replay ring, debug mirror.
 * With no sink registered the line goes to the debug backend and is otherwise
 * dropped. The sink may edit the line in place (it strips a trailing newline).
 *
 * Context rules (the sink runs a foreign hook under Forbid): task context
 * only — never from an interrupt — and never inside Disable(). Under the core
 * lock or inside Forbid() is fine.
 */
#define NS_LOG_EMERG 0
#define NS_LOG_ALERT 1
#define NS_LOG_CRIT 2
#define NS_LOG_ERR 3
#define NS_LOG_WARNING 4
#define NS_LOG_NOTICE 5
#define NS_LOG_INFO 6
#define NS_LOG_DEBUG 7

typedef void (*netstack_log_sink_fn)(int pri, char *text);

void netstack_log_set_sink(netstack_log_sink_fn fn);
void netstack_log(int pri, const char *fmt, ...);

#endif /* LWIPAMIGA_NETSTACK_DIAG_H */
