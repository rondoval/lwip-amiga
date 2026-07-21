/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Reverse DNS (address -> hostname). lwIP's resolver (dns.c) is forward-only,
 * so a PTR lookup is a one-shot UDP transaction we run ourselves: build the
 * d.c.b.a.in-addr.arpa query, send it to the configured resolver, and parse
 * the PTR answer.
 *
 * Like every API path this runs under the netstack core lock; the udp_recv
 * callback (driver RX context, already under the lock) stashes the reply and
 * signals the opener, which is parked in sb_wait_to with a hard timeout.
 */

#include "sb_base.h"

#include <lwip/dns.h>
#include <lwip/ip4_addr.h>
#include <lwip/ip_addr.h>
#include <lwip/pbuf.h>
#include <lwip/udp.h>

#include <debug.h>

#include "netstack.h"

#define SB_RDNS_PORT 53
#define SB_RDNS_TIMEOUT_MS 5000
#define SB_RDNS_MAXPKT 512

#define SB_DNS_TYPE_PTR 12
#define SB_DNS_CLASS_IN 1

struct sb_rdns_ctx
{
    struct SocketBase *base;
    struct pbuf *reply;
};

static void sb_rdns_recv_cb(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                            const ip_addr_t *addr, u16_t port)
{
    struct sb_rdns_ctx *ctx = arg;
    (void)pcb;
    (void)addr;
    (void)port;

    if (ctx == NULL || p == NULL)
    {
        if (p != NULL)
            pbuf_free(p);
        return;
    }
    if (ctx->reply != NULL)
    {
        pbuf_free(p); /* first reply wins; drop any stragglers */
        return;
    }
    ctx->reply = p;
    if (ctx->base->task != NULL)
        Signal(ctx->base->task, 1UL << ctx->base->sigBit);
}

/* Append the decimal form of @octet as one length-prefixed label. */
static LONG sb_dns_put_octet(UBYTE *buf, LONG pos, LONG max, UBYTE octet)
{
    char tmp[3];
    LONG n = 0;
    do
    {
        tmp[n++] = (char)('0' + octet % 10);
        octet /= 10;
    } while (octet != 0);
    if (pos + 1 + n > max)
        return -1;
    buf[pos++] = (UBYTE)n;
    while (n > 0)
        buf[pos++] = (UBYTE)tmp[--n];
    return pos;
}

/* Append a literal label (e.g. "arpa"). @label is NUL-terminated, < 64 bytes. */
static LONG sb_dns_put_label(UBYTE *buf, LONG pos, LONG max, const char *label)
{
    LONG n = 0;
    while (label[n] != '\0')
        n++;
    if (pos + 1 + n > max)
        return -1;
    buf[pos++] = (UBYTE)n;
    for (LONG i = 0; i < n; i++)
        buf[pos++] = (UBYTE)label[i];
    return pos;
}

/* Build a PTR query for @addr (network order) into @buf. Returns the length or
 * -1 if it would not fit. */
static LONG sb_rdns_build(UBYTE *buf, LONG max, ULONG addr, UWORD id)
{
    if (max < 12)
        return -1;
    buf[0] = (UBYTE)(id >> 8);
    buf[1] = (UBYTE)id;
    buf[2] = 0x01; /* flags: recursion desired */
    buf[3] = 0x00;
    buf[4] = 0x00;
    buf[5] = 0x01; /* qdcount = 1 */
    buf[6] = buf[7] = buf[8] = buf[9] = buf[10] = buf[11] = 0;

    LONG pos = 12;
    /* d.c.b.a order (least-significant octet first) */
    pos = sb_dns_put_octet(buf, pos, max, (UBYTE)(addr & 0xFF));
    if (pos >= 0)
        pos = sb_dns_put_octet(buf, pos, max, (UBYTE)((addr >> 8) & 0xFF));
    if (pos >= 0)
        pos = sb_dns_put_octet(buf, pos, max, (UBYTE)((addr >> 16) & 0xFF));
    if (pos >= 0)
        pos = sb_dns_put_octet(buf, pos, max, (UBYTE)((addr >> 24) & 0xFF));
    if (pos >= 0)
        pos = sb_dns_put_label(buf, pos, max, "in-addr");
    if (pos >= 0)
        pos = sb_dns_put_label(buf, pos, max, "arpa");
    if (pos < 0 || pos + 5 > max)
        return -1;
    buf[pos++] = 0; /* root label terminates the name */
    buf[pos++] = (UBYTE)(SB_DNS_TYPE_PTR >> 8);
    buf[pos++] = (UBYTE)SB_DNS_TYPE_PTR;
    buf[pos++] = (UBYTE)(SB_DNS_CLASS_IN >> 8);
    buf[pos++] = (UBYTE)SB_DNS_CLASS_IN;
    return pos;
}

/* Advance past a (possibly compressed) name; returns the offset just after it
 * or -1 on malformed input. */
static LONG sb_dns_skip_name(const UBYTE *pkt, LONG len, LONG pos)
{
    while (pos >= 0 && pos < len)
    {
        UBYTE b = pkt[pos];
        if ((b & 0xC0) == 0xC0)
            return pos + 2 <= len ? pos + 2 : -1;
        if (b == 0)
            return pos + 1;
        pos += 1 + b;
    }
    return -1;
}

/* Decode a (possibly compressed) name at @pos into @out as a dotted string.
 * Returns 0 on success, -1 on malformed input or overflow. */
static LONG sb_dns_name(const UBYTE *pkt, LONG len, LONG pos, char *out, LONG outmax)
{
    LONG o = 0;
    LONG guard = 0;

    while (pos >= 0 && pos < len)
    {
        UBYTE b = pkt[pos];
        if ((b & 0xC0) == 0xC0)
        {
            if (pos + 1 >= len)
                return -1;
            pos = ((b & 0x3F) << 8) | pkt[pos + 1];
            if (++guard > 128)
                return -1; /* pointer loop */
            continue;
        }
        if (b == 0)
            break;
        pos++;
        if (b > 63 || pos + b > len)
            return -1;
        if (o != 0 && o < outmax - 1)
            out[o++] = '.';
        for (LONG i = 0; i < b; i++)
            if (o < outmax - 1)
                out[o++] = (char)pkt[pos + i];
        pos += b;
        if (++guard > 128)
            return -1;
    }
    if (outmax <= 0)
        return -1;
    out[o < outmax ? o : outmax - 1] = '\0';
    return o > 0 ? 0 : -1;
}

/* Parse a DNS response, extracting the first PTR answer into @out. */
static LONG sb_rdns_parse(const UBYTE *pkt, LONG len, UWORD id, char *out, LONG outmax)
{
    if (len < 12)
        return -1;
    if (((pkt[0] << 8) | pkt[1]) != id)
        return -1;
    if ((pkt[2] & 0x80) == 0)
        return -1; /* not a response */
    if ((pkt[3] & 0x0F) != 0)
        return -1; /* RCODE != 0 */

    UWORD qd = (UWORD)((pkt[4] << 8) | pkt[5]);
    UWORD an = (UWORD)((pkt[6] << 8) | pkt[7]);
    if (an == 0)
        return -1;

    LONG pos = 12;
    for (UWORD i = 0; i < qd; i++)
    {
        pos = sb_dns_skip_name(pkt, len, pos);
        if (pos < 0 || pos + 4 > len)
            return -1;
        pos += 4; /* qtype + qclass */
    }

    for (UWORD i = 0; i < an; i++)
    {
        pos = sb_dns_skip_name(pkt, len, pos);
        if (pos < 0 || pos + 10 > len)
            return -1;
        UWORD type = (UWORD)((pkt[pos] << 8) | pkt[pos + 1]);
        UWORD rdlen = (UWORD)((pkt[pos + 8] << 8) | pkt[pos + 9]);
        LONG rdata = pos + 10;
        if (rdata + rdlen > len)
            return -1;
        if (type == SB_DNS_TYPE_PTR)
            return sb_dns_name(pkt, len, rdata, out, outmax);
        pos = rdata + rdlen;
    }
    return -1;
}

LONG sb_ptr_resolve(struct SocketBase *base, ULONG addr, char *out, ULONG outmax)
{
    KprintfT("[bsdsocket] %s: addr=0x%08lx\n", __func__, addr);
    if (out == NULL || outmax == 0)
        return -1;

    netstack_lock();

    const ip_addr_t *srv = dns_getserver(0);
    if (srv == NULL || ip_addr_isany(srv))
    {
        netstack_unlock();
        return -1;
    }
    ip_addr_t server = *srv;

    struct udp_pcb *pcb = udp_new();
    if (pcb == NULL)
    {
        netstack_unlock();
        return -1;
    }
    if (udp_bind(pcb, IP_ADDR_ANY, 0) != ERR_OK)
    {
        udp_remove(pcb);
        netstack_unlock();
        return -1;
    }

    struct sb_rdns_ctx ctx;
    ctx.base = base;
    ctx.reply = NULL;
    udp_recv(pcb, sb_rdns_recv_cb, &ctx);

    UWORD id = (UWORD)netstack_now_ms();
    UBYTE qbuf[SB_RDNS_MAXPKT];
    LONG qlen = sb_rdns_build(qbuf, (LONG)sizeof(qbuf), addr, id);
    struct pbuf *q = qlen > 0 ? pbuf_alloc(PBUF_TRANSPORT, (u16_t)qlen, PBUF_RAM) : NULL;
    if (q == NULL)
    {
        udp_recv(pcb, NULL, NULL);
        udp_remove(pcb);
        netstack_unlock();
        return -1;
    }
    pbuf_take(q, qbuf, (u16_t)qlen);
    err_t sr = udp_sendto(pcb, q, &server, SB_RDNS_PORT);
    pbuf_free(q);

    LONG rc = -1;
    if (sr == ERR_OK)
    {
        struct SbTimedWait tw;
        tw.deadlineMs = 0;
        tw.set = FALSE;
        while (ctx.reply == NULL)
        {
            if (sb_wait_to(base, SB_RDNS_TIMEOUT_MS, &tw) != 0)
                break; /* timeout (EWOULDBLOCK) or CTRL-C (EINTR) */
        }
    }

    if (ctx.reply != NULL)
    {
        UBYTE pkt[SB_RDNS_MAXPKT];
        u16_t plen = ctx.reply->tot_len;
        if (plen > sizeof(pkt))
            plen = sizeof(pkt);
        pbuf_copy_partial(ctx.reply, pkt, plen, 0);
        pbuf_free(ctx.reply);
        ctx.reply = NULL;
        if (sb_rdns_parse(pkt, (LONG)plen, id, out, (LONG)outmax) == 0)
            rc = 0;
    }

    udp_recv(pcb, NULL, NULL);
    udp_remove(pcb);
    netstack_unlock();
    return rc;
}
