/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * vsyslog — the library-side syslog sink. Honours the per-opener SBTC_LOG*
 * configuration (ident tag, priority mask) and routes the formatted message
 * to the debug backend.
 */

#include "sb_base.h"

#include <debug.h>

/* Minimal C-style formatter: bebbo varargs promote every integer to 32 bits,
 * so %d/%u/%x and their l-variants all read one long word; %s reads a
 * pointer. Anything else is copied through. */
static ULONG sb_vformat(char *dst, ULONG max, const char *fmt, const ULONG *args)
{
    KprintfT("[bsdsocket] %s: fmt=%s\n", __func__, (ULONG)fmt);
    ULONG o = 0;
    ULONG ai = 0;

    while (*fmt != '\0' && o < max - 1)
    {
        if (*fmt != '%')
        {
            dst[o++] = *fmt++;
            continue;
        }
        fmt++;

        /* %[-][0][width][.prec] — flags, then the length modifiers we accept
         * but do not act on (all varargs cells are promoted to 32-bit). */
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
        while (*fmt == 'l' || *fmt == 'h')
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
            const char *sp = (const char *)args[ai++];
            if (sp == NULL)
                sp = "(null)";
            body = sp;
            while (sp[blen] != '\0' && (prec < 0 || blen < prec))
                blen++;
            pad = ' '; /* strings never zero-pad */
            break;
        }
        case 'c':
            tmp[blen++] = (char)args[ai++];
            break;
        case 'd':
        case 'i':
        {
            LONG v = (LONG)args[ai++];
            BOOL neg = v < 0;
            ULONG uv = neg ? (ULONG)(-v) : (ULONG)v;
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
            ULONG v = args[ai++];
            ULONG radix = (conv == 'u') ? 10 : 16;
            char digs[12];
            int t = 0;
            do
            {
                ULONG digit = v % radix;
                digs[t++] = (char)(digit < 10 ? '0' + digit : 'a' + digit - 10);
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

VOID bsd_vsyslog(LONG pri asm("d0"), STRPTR msg asm("a0"), APTR args asm("a1"),
                 struct SocketBase *base asm("a6"))
{
    char buf[256];

    if (msg == NULL)
        return;
    /* drop priorities the opener masked off via SBTC_LOGMASK (setlogmask) */
    if (base != NULL && !(base->logMask & SB_LOG_MASK(pri)))
        return;
    sb_vformat(buf, sizeof(buf), (const char *)msg, args);
    if (base != NULL && base->logTagPtr != NULL)
        Kprintf("[syslog:%ld] %s: %s\n", pri, (ULONG)base->logTagPtr, (ULONG)buf);
    else
        Kprintf("[syslog:%ld] %s\n", pri, (ULONG)buf);
}
