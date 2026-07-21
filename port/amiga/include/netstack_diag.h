/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Diag formatting for the port layer (implemented in netstack_diag.c).
 *
 * Both entry points format with C argument promotion — 32-bit %d/%u/%x, %p —
 * which RawDoFmt (behind Kprintf) cannot do: it reads bare %d/%u/%x as 16-bit
 * and has no %p. Only the finished string reaches the debug backend. Use them
 * ONLY for format strings that need those semantics; everything else should
 * call Kprintf/KprintfT directly and follow the RawDoFmt conventions
 * (0x%08lx for pointers, explicit ULONG/LONG casts).
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
 * Writes at most max-1 chars plus a NUL; returns the number written.
 *
 * Debug-tier only — there is deliberately no below-tier stub, so its caller
 * must guard the call rather than format into a discarded buffer. */
unsigned long netstack_vformat_args(char *dst, unsigned long max,
                                    const char *fmt, const unsigned long *args);

#endif /* LWIPAMIGA_NETSTACK_DIAG_H */
