/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * lwIP diag output. lwIP's diag format strings pass 32-bit values as plain
 * %d/%u/%x, which RawDoFmt would read as 16-bit — so the formatting
 * happens here with C argument promotion and only the finished string
 * reaches the backend.
 *
 * Debug tier. The entry point stays defined at every tier (lwIP needs
 * LWIP_PLATFORM_DIAG to exist, and netstack_platform_diag must still halt)
 * but its body compiles out below it, so a build with no sink is completely
 * silent. Hot paths never call this.
 */

#include "netstack_sys.h"

#include <stdarg.h>
#include <stddef.h> /* size_t, for the freestanding snprintf below */

#include <debug.h>

/* Covers the conversions lwIP uses (c s p d i u x X, with -/0/width/h/l/z);
 * anything else prints literally. */
#define NS_DIAG_BUF 256

#ifdef DEBUG

static void ns_diag_putc(char *buf, ULONG *pos, ULONG cap, char c)
{
    if (*pos < cap - 1)
        buf[(*pos)++] = c;
}

/* The shared formatter: writes at most cap-1 chars plus a NUL and returns the
 * length written. Backs both netstack_diag_printf and the freestanding
 * snprintf below. */
static ULONG ns_vformat(char *buf, ULONG cap, const char *fmt, va_list ap)
{
    ULONG pos = 0;
    if (cap == 0)
        return 0;
    for (; *fmt != '\0'; fmt++)
    {
        if (*fmt != '%')
        {
            ns_diag_putc(buf, &pos, cap, *fmt);
            continue;
        }

        fmt++;
        char pad = ' ';
        if (*fmt == '-')
            fmt++; /* left-align: not worth honoring in a log */
        if (*fmt == '0')
        {
            pad = '0';
            fmt++;
        }
        ULONG width = 0;
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (ULONG)(*fmt++ - '0');
        while (*fmt == 'h' || *fmt == 'l' || *fmt == 'z')
            fmt++; /* every integer arrives 32-bit after promotion */

        char conv = *fmt;
        if (conv == '\0')
            break;
        if (conv == '%')
        {
            ns_diag_putc(buf, &pos, cap, '%');
            continue;
        }
        if (conv == 'c')
        {
            ns_diag_putc(buf, &pos, cap, (char)va_arg(ap, int));
            continue;
        }
        if (conv == 's')
        {
            const char *s = va_arg(ap, const char *);
            if (s == NULL)
                s = "(null)";
            for (; *s != '\0'; s++)
                ns_diag_putc(buf, &pos, cap, *s);
            continue;
        }

        ULONG val;
        ULONG base;
        const char *dig = (conv == 'X') ? "0123456789ABCDEF" : "0123456789abcdef";
        BOOL negative = FALSE;
        switch (conv)
        {
        case 'd':
        case 'i':
        {
            LONG sv = va_arg(ap, LONG);
            negative = sv < 0;
            val = negative ? (ULONG)0 - (ULONG)sv : (ULONG)sv;
            base = 10;
            break;
        }
        case 'u':
            val = va_arg(ap, ULONG);
            base = 10;
            break;
        case 'x':
        case 'X':
            val = va_arg(ap, ULONG);
            base = 16;
            break;
        case 'p':
            val = (ULONG)va_arg(ap, void *);
            base = 16;
            pad = '0';
            width = 8;
            break;
        default:
            /* unknown conversion: emit literally, don't touch the va_list */
            ns_diag_putc(buf, &pos, cap, '%');
            ns_diag_putc(buf, &pos, cap, conv);
            continue;
        }

        char tmp[11]; /* 32-bit decimal maxes at 10 digits, +1 for '-' */
        ULONG n = 0;
        do
        {
            tmp[n++] = dig[val % base];
            val /= base;
        } while (val != 0);
        if (negative)
            tmp[n++] = '-';
        for (; width > n; width--)
            ns_diag_putc(buf, &pos, cap, pad);
        while (n > 0)
            ns_diag_putc(buf, &pos, cap, tmp[--n]);
    }

    buf[pos] = '\0';
    return pos;
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
 * stdio to satisfy it fails the link, because a ROM-able library has no C
 * startup to supply SysBase/exit. This covers only the small format subset
 * lwIP uses — it is not a complete snprintf. */
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

#endif /* DEBUG */
