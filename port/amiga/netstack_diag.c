/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The component's C-standard formatter. lwIP's diag format strings pass 32-bit
 * values as plain %d/%u/%x and use %p, which RawDoFmt would read as 16-bit and
 * cannot render at all — so the formatting happens here with C argument
 * promotion and only the finished string reaches the backend.
 *
 * One core, two argument sources (both flat 32-bit cells after promotion):
 * varargs for LWIP_PLATFORM_DIAG and the freestanding snprintf, and a ULONG
 * array for bsd_vsyslog's AmigaOS calling convention.
 *
 * Debug tier. netstack_diag_printf stays defined at every tier (lwIP needs
 * LWIP_PLATFORM_DIAG to resolve) but its body compiles out below it, so a
 * build with no sink is completely silent. Hot paths never call this.
 */

#include "netstack_sys.h"

#include <stdarg.h>
#include <stddef.h> /* size_t, for the freestanding snprintf below */

#include <debug.h>

#include "netstack_diag.h"

#define NS_DIAG_BUF 256

#ifdef DEBUG

/* Argument source: both callers hand over a sequence of 32-bit cells, so the
 * core pulls each one through this and stays agnostic about where they live.
 * ULONG, not unsigned long: identical on m68k, but it keeps the va_arg read
 * one cell wide if this is ever compiled for a host test harness. */
typedef ULONG (*ns_fmt_next)(void *ctx);

/*
 * Writes at most max-1 chars plus a NUL; returns the number written (not the
 * length that a full buffer would have taken — callers only log the result).
 *
 * Covers %[-][0][width][.prec][h|l|z] with conversions c s p d i u x X %.
 * An unknown conversion is emitted literally and consumes no argument, so one
 * bad specifier cannot desynchronise the rest of the line.
 */
static ULONG ns_vformat_core(char *dst, ULONG max, const char *fmt,
                             ns_fmt_next next, void *ctx)
{
    ULONG o = 0;

    if (max == 0)
        return 0;

    while (*fmt != '\0' && o < max - 1)
    {
        if (*fmt != '%')
        {
            dst[o++] = *fmt++;
            continue;
        }
        fmt++;

        BOOL leftJust = FALSE;
        BOOL zeroPad = FALSE;
        while (*fmt == '-' || *fmt == '0')
        {
            if (*fmt == '-')
                leftJust = TRUE;
            else
                zeroPad = TRUE;
            fmt++;
        }
        LONG width = 0;
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');
        LONG prec = -1;
        if (*fmt == '.')
        {
            fmt++;
            prec = 0;
            while (*fmt >= '0' && *fmt <= '9')
                prec = prec * 10 + (*fmt++ - '0');
        }
        /* accepted and ignored: every cell arrives 32-bit after promotion */
        while (*fmt == 'l' || *fmt == 'h' || *fmt == 'z')
            fmt++;

        /* Format the converted body into tmp, then emit it padded to width.
         * pad = '0' only for right-justified numeric conversions. */
        char tmp[24];
        const char *body = tmp;
        LONG blen = 0;
        char pad = (zeroPad && !leftJust) ? '0' : ' ';

        char conv = *fmt != '\0' ? *fmt++ : '\0';
        switch (conv)
        {
        case 's':
        {
            const char *sp = (const char *)next(ctx);
            if (sp == NULL)
                sp = "(null)";
            body = sp;
            while (sp[blen] != '\0' && (prec < 0 || blen < prec))
                blen++;
            pad = ' '; /* strings never zero-pad */
            break;
        }
        case 'c':
            tmp[blen++] = (char)next(ctx);
            break;
        case 'd':
        case 'i':
        {
            LONG v = (LONG)next(ctx);
            BOOL neg = v < 0;
            /* negate in unsigned space so LONG_MIN doesn't overflow */
            ULONG uv = neg ? (ULONG)0 - (ULONG)v : (ULONG)v;
            char digs[12];
            int t = 0;
            do
            {
                digs[t++] = (char)('0' + (uv % 10));
                uv /= 10;
            } while (uv != 0);
            if (neg)
                tmp[blen++] = '-';
            while (t > 0)
                tmp[blen++] = digs[--t];
            break;
        }
        case 'u':
        case 'x':
        case 'X':
        case 'p':
        {
            ULONG v = (ULONG)next(ctx);
            ULONG radix = (conv == 'u') ? 10 : 16;
            const char *dig = (conv == 'X') ? "0123456789ABCDEF"
                                            : "0123456789abcdef";
            char digs[12];
            int t = 0;

            /* %p is a fixed 8-digit zero-padded address: it lines up in a dump
             * and matches how the fleet's post-mortem tooling reads pointers. */
            if (conv == 'p')
            {
                pad = '0';
                if (width < 8)
                    width = 8;
            }
            do
            {
                digs[t++] = dig[v % radix];
                v /= radix;
            } while (v != 0);
            while (t > 0)
                tmp[blen++] = digs[--t];
            break;
        }
        case '%':
            tmp[blen++] = '%';
            break;
        default:
            tmp[blen++] = '%';
            if (conv != '\0')
                tmp[blen++] = conv;
            break;
        }

        LONG padlen = width - blen;
        if (!leftJust)
        {
            /* zero-fill goes between the sign and the digits: "%05d" of -12
             * is "-0012", never "00-12" (the sign still counts toward width) */
            if (pad == '0' && blen > 0 && body[0] == '-' && o < max - 1)
            {
                dst[o++] = '-';
                body++;
                blen--;
            }
            while (padlen-- > 0 && o < max - 1)
                dst[o++] = pad;
        }
        for (LONG i = 0; i < blen && o < max - 1; i++)
            dst[o++] = body[i];
        if (leftJust)
            while (padlen-- > 0 && o < max - 1)
                dst[o++] = ' ';
    }

    dst[o] = '\0';
    return o;
}

/* --- varargs front end (LWIP_PLATFORM_DIAG, snprintf) --- */

/* va_list lives in a struct so a pointer to it can cross the accessor
 * boundary: va_list may be an array type, which cannot simply be assigned. */
struct ns_fmt_va
{
    va_list ap;
};

static ULONG ns_next_va(void *ctx)
{
    return va_arg(((struct ns_fmt_va *)ctx)->ap, ULONG);
}

static ULONG ns_vformat(char *dst, ULONG max, const char *fmt, va_list ap)
{
    struct ns_fmt_va v;
    ULONG n;

    va_copy(v.ap, ap);
    n = ns_vformat_core(dst, max, fmt, ns_next_va, &v);
    va_end(v.ap);
    return n;
}

/* --- array front end (bsd_vsyslog) --- */

struct ns_fmt_vec
{
    const unsigned long *args;
    unsigned long i;
};

static ULONG ns_next_vec(void *ctx)
{
    struct ns_fmt_vec *v = (struct ns_fmt_vec *)ctx;
    return (ULONG)v->args[v->i++];
}

unsigned long netstack_vformat_args(char *dst, unsigned long max,
                                    const char *fmt, const unsigned long *args)
{
    struct ns_fmt_vec v;

    v.args = args;
    v.i = 0;
    return ns_vformat_core(dst, (ULONG)max, fmt, ns_next_vec, &v);
}

/* LWIP_PLATFORM_DIAG (see lwipopts.h) */
void netstack_diag_printf(const char *fmt, ...)
{
    char buf[NS_DIAG_BUF];
    va_list ap;

    va_start(ap, fmt);
    ns_vformat(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    Kprintf("%s", buf); /* respect the selected debug backend */
}

#ifdef TRACE
/* Freestanding snprintf. lwIP formats an assert message with snprintf when
 * MEMP_OVERFLOW_CHECK is on, which is the TRACE tier; pulling libnix's
 * stdio to satisfy it fails the link, because a -nostartfiles library
 * binary carries no C startup to supply SysBase/exit. This covers only the
 * small format subset lwIP uses — it is not a complete snprintf (notably it
 * returns the length written, not the length required). */
int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    ULONG n;

    va_start(ap, fmt);
    n = ns_vformat(buf, (ULONG)size, fmt, ap);
    va_end(ap);
    return (int)n;
}
#endif /* TRACE */

#else /* !DEBUG: keep the entry point, drop the output */

void netstack_diag_printf(const char *fmt, ...)
{
    (void)fmt;
}

/* netstack_vformat_args has no stub: its only caller (bsd_vsyslog) guards the
 * call itself, so below the debug tier it never formats into a discarded
 * buffer — while the LVO stays present and callable. */

#endif /* DEBUG */
