/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * inet_* address conversion (dotted-quad parsing/printing, the classful
 * legacy helpers, inet_network) and address classification
 * (In_LocalAddr / In_CanForward).
 */

#include "sb_base.h"

#include <lwip/ip4_addr.h>
#include <lwip/netif.h>

#include <debug.h>

#include "netstack.h"

STRPTR bsd_Inet_NtoA(ULONG ip asm("d0"), struct SocketBase *base asm("a6"))
{
    KprintfT("[bsdsocket] %s: ip=0x%08lx\n", __func__, ip);
    ip4_addr_t a;
    ip4_addr_set_u32(&a, ip);
    ip4addr_ntoa_r(&a, base->ntoaBuf, sizeof(base->ntoaBuf));
    return (STRPTR)base->ntoaBuf;
}

ULONG bsd_inet_addr(STRPTR cp asm("a0"), struct SocketBase *base asm("a6"))
{
    KprintfT("[bsdsocket] %s: cp=%s\n", __func__, cp != NULL ? (ULONG)cp : (ULONG)"(null)");
    (void)base;
    ip4_addr_t a;
    if (cp == NULL || ip4addr_aton((const char *)cp, &a) == 0)
        return 0xFFFFFFFF; /* INADDR_NONE */
    return ip4_addr_get_u32(&a);
}

LONG bsd_inet_aton(STRPTR cp asm("a0"), APTR addr asm("a1"),
                   struct SocketBase *base asm("a6"))
{
    KprintfT("[bsdsocket] %s: cp=%s\n", __func__, cp != NULL ? (ULONG)cp : (ULONG)"(null)");
    (void)base;
    ip4_addr_t a;
    if (cp == NULL || addr == NULL || ip4addr_aton((const char *)cp, &a) == 0)
        return 0;
    *(ULONG *)addr = ip4_addr_get_u32(&a);
    return 1;
}

STRPTR bsd_inet_ntop(LONG af asm("d0"), APTR src asm("a0"), STRPTR dst asm("a1"),
                     LONG size asm("d1"), struct SocketBase *base asm("a6"))
{
    KprintfT("[bsdsocket] %s: af=%ld size=%ld\n", __func__, af, size);
    if (af != SB_AF_INET || src == NULL || dst == NULL)
    {
        sb_set_errno(base, SB_EAFNOSUPPORT);
        return NULL;
    }
    ip4_addr_t a;
    ip4_addr_set_u32(&a, *(ULONG *)src);
    if (ip4addr_ntoa_r(&a, (char *)dst, (int)size) == NULL)
    {
        sb_set_errno(base, SB_EMSGSIZE);
        return NULL;
    }
    return dst;
}

LONG bsd_inet_pton(LONG af asm("d0"), STRPTR src asm("a0"), APTR dst asm("a1"),
                   struct SocketBase *base asm("a6"))
{
    KprintfT("[bsdsocket] %s: af=%ld src=%s\n", __func__, af, src != NULL ? (ULONG)src : (ULONG)"(null)");
    if (af != SB_AF_INET)
    {
        sb_set_errno(base, SB_EAFNOSUPPORT);
        return -1;
    }
    ip4_addr_t a;
    if (src == NULL || dst == NULL || ip4addr_aton((const char *)src, &a) == 0)
        return 0;
    *(ULONG *)dst = ip4_addr_get_u32(&a);
    return 1;
}

/* classful helpers — legacy API surface, exact 4.3BSD semantics */
ULONG bsd_Inet_LnaOf(ULONG in asm("d0"), struct SocketBase *base asm("a6"))
{
    KprintfT("[bsdsocket] %s: in=0x%08lx\n", __func__, in);
    (void)base;
    if ((in & 0x80000000UL) == 0)
        return in & 0x00FFFFFF;
    if ((in & 0xC0000000UL) == 0x80000000UL)
        return in & 0x0000FFFF;
    return in & 0x000000FF;
}

ULONG bsd_Inet_NetOf(ULONG in asm("d0"), struct SocketBase *base asm("a6"))
{
    KprintfT("[bsdsocket] %s: in=0x%08lx\n", __func__, in);
    (void)base;
    if ((in & 0x80000000UL) == 0)
        return (in >> 24) & 0xFF;
    if ((in & 0xC0000000UL) == 0x80000000UL)
        return (in >> 16) & 0xFFFF;
    return (in >> 8) & 0xFFFFFF;
}

ULONG bsd_Inet_MakeAddr(ULONG net asm("d0"), ULONG host asm("d1"),
                        struct SocketBase *base asm("a6"))
{
    KprintfT("[bsdsocket] %s: net=0x%08lx host=0x%08lx\n", __func__, net, host);
    (void)base;
    if (net < 128)
        return (net << 24) | (host & 0x00FFFFFF);
    if (net < 65536)
        return (net << 16) | (host & 0x0000FFFF);
    return (net << 8) | (host & 0x000000FF);
}

ULONG bsd_inet_network(STRPTR cp asm("a0"), struct SocketBase *base asm("a6"))
{
    KprintfT("[bsdsocket] %s: cp=%s\n", __func__, cp != NULL ? (ULONG)cp : (ULONG)"(null)");
    (void)base;
    if (cp == NULL)
        return 0xFFFFFFFF; /* INADDR_NONE */

    /* Classic 4.4BSD inet_network(): up to four dot-separated parts, each in
     * C notation (leading 0x hex, leading 0 octal, else decimal). Each part
     * contributes one byte, packed right-aligned into the low bytes in host
     * order — "192.168" -> 0xC0A8, "10.0.0.0" -> 0x0A000000, "127" -> 0x7F.
     * Parts are masked to a byte (BSD behaviour), not rejected when > 255. */
    const char *s = (const char *)cp;
    ULONG parts[4];
    ULONG nparts = 0;

    for (;;)
    {
        ULONG radix = 10;
        if (*s == '0')
        {
            radix = 8;
            s++;
            if (*s == 'x' || *s == 'X')
            {
                radix = 16;
                s++;
            }
        }

        ULONG val = 0;
        BOOL any = (radix != 10); /* a consumed leading '0' is itself digit 0 */
        for (;;)
        {
            char c = *s;
            ULONG digit;
            if (c >= '0' && c <= '9')
                digit = (ULONG)(c - '0');
            else if (radix == 16 && c >= 'a' && c <= 'f')
                digit = (ULONG)(c - 'a' + 10);
            else if (radix == 16 && c >= 'A' && c <= 'F')
                digit = (ULONG)(c - 'A' + 10);
            else
                break;
            if (digit >= radix)
                break;
            val = val * radix + digit;
            any = TRUE;
            s++;
        }

        if (!any || nparts >= 4)
            return 0xFFFFFFFF;
        parts[nparts++] = val;

        if (*s != '.')
            break;
        s++;
    }

    while (*s == ' ' || *s == '\t')
        s++;
    if (*s != '\0')
        return 0xFFFFFFFF;

    ULONG result = 0;
    for (ULONG i = 0; i < nparts; i++)
        result = (result << 8) | (parts[i] & 0xFF);
    return result;
}

/* ------------------------------------------------ address classification --- */

LONG bsd_In_LocalAddr(ULONG address asm("d0"), struct SocketBase *base asm("a6"))
{
    KprintfT("[bsdsocket] %s: addr=0x%08lx\n", __func__, address);
    (void)base;
    LONG local = 0;

    netstack_lock();
    struct netif *nif;
    NETIF_FOREACH(nif)
    {
        ULONG a = ip4_addr_get_u32(netif_ip4_addr(nif));
        ULONG m = ip4_addr_get_u32(netif_ip4_netmask(nif));
        if (a != 0 && (address & m) == (a & m))
        {
            local = 1;
            break;
        }
    }
    netstack_unlock();
    return local;
}

LONG bsd_In_CanForward(ULONG address asm("d0"), struct SocketBase *base asm("a6"))
{
    KprintfT("[bsdsocket] %s: addr=0x%08lx\n", __func__, address);
    (void)base;
    /* 4.3BSD in_canforward(): a datagram to this address may be forwarded
     * unless it is class D (multicast) or class E (experimental) space, or a
     * class A datagram for net 0 or net 127 (loopback). The address arrives
     * as a network-order u32; on big-endian 68k that equals the host-order
     * integer used by the classful masks, so no byte swap is needed. */
    if ((address & 0xF0000000UL) == 0xE0000000UL || /* class D multicast */
        (address & 0xF0000000UL) == 0xF0000000UL)   /* class E / experimental */
        return 0;
    if ((address & 0x80000000UL) == 0) /* class A */
    {
        ULONG net = address & 0xFF000000UL;
        if (net == 0 || net == (127UL << 24))
            return 0;
    }
    return 1;
}
