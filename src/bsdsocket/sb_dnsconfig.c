/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Resolver configuration: the Roadshow DNS-server list LVOs over lwIP's
 * dns_setserver slots, and the default (search) domain used by the
 * sb_resolver.c qualified-retry.
 */

#include "sb_base.h"

#include <minlist.h>

#include <lwip/dns.h>
#include <lwip/ip4_addr.h>

#include <debug.h>

#include "netstack.h"

LONG bsd_AddDomainNameServer(STRPTR address asm("a0"), struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s: address=%s\n", __func__, address != NULL ? (ULONG)address : (ULONG)"(null)");
    ip_addr_t ip;

    if (address == NULL || ip4addr_aton((const char *)address, ip_2_ip4(&ip)) == 0)
    {
        sb_set_errno(base, SB_EINVAL);
        return -1;
    }
    IP_SET_TYPE(&ip, IPADDR_TYPE_V4);

    LONG result = -1;
    netstack_lock();
    for (u8_t i = 0; i < DNS_MAX_SERVERS; i++)
    {
        const ip_addr_t *cur = dns_getserver(i);
        if (ip_addr_cmp(cur, &ip))
        {
            result = 0; /* already present */
            break;
        }
        if (ip_addr_isany(cur))
        {
            dns_setserver(i, &ip);
            result = 0;
            break;
        }
    }
    netstack_unlock();

    if (result != 0)
        sb_set_errno(base, SB_ENOBUFS);
    return result;
}

LONG bsd_RemoveDomainNameServer(STRPTR address asm("a0"), struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s: address=%s\n", __func__, address != NULL ? (ULONG)address : (ULONG)"(null)");
    ip_addr_t ip;

    if (address == NULL || ip4addr_aton((const char *)address, ip_2_ip4(&ip)) == 0)
    {
        sb_set_errno(base, SB_EINVAL);
        return -1;
    }
    IP_SET_TYPE(&ip, IPADDR_TYPE_V4);

    LONG result = -1;
    netstack_lock();
    for (u8_t i = 0; i < DNS_MAX_SERVERS; i++)
    {
        if (ip_addr_cmp(dns_getserver(i), &ip))
        {
            dns_setserver(i, NULL);
            result = 0;
            break;
        }
    }
    netstack_unlock();

    if (result != 0)
        sb_set_errno(base, SB_EINVAL);
    return result;
}

struct sb_dns_list
{
    struct MinList list; /* of sb_DomainNameServerNode */
};

APTR bsd_ObtainDomainNameServerList(struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s\n", __func__);
    struct sb_dns_list *l = AllocVec(sizeof(struct sb_dns_list), MEMF_PUBLIC);
    if (l == NULL)
    {
        sb_set_errno(base, SB_ENOBUFS);
        return NULL;
    }
    _NewMinList(&l->list);

    netstack_lock();
    for (u8_t i = 0; i < DNS_MAX_SERVERS; i++)
    {
        const ip_addr_t *cur = dns_getserver(i);
        if (ip_addr_isany(cur))
            continue;

        struct sb_DomainNameServerNode *n =
            AllocVec(sizeof(*n) + 20, MEMF_PUBLIC | MEMF_CLEAR);
        if (n == NULL)
            break;
        n->dnsn_Size = sizeof(*n);
        n->dnsn_Address = (STRPTR)(n + 1);
        n->dnsn_UseCount = 1;
        ip4addr_ntoa_r(ip_2_ip4(cur), (char *)n->dnsn_Address, 20);
        AddTailMinList(&l->list, &n->dnsn_MinNode);
    }
    netstack_unlock();
    return l;
}

VOID bsd_ReleaseDomainNameServerList(APTR list asm("a0"), struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s: list=0x%08lx\n", __func__, (ULONG)list);
    struct sb_dns_list *l = list;
    (void)base;

    if (l == NULL)
        return;

    struct MinNode *n;
    while ((n = RemHeadMinList(&l->list)) != NULL)
        FreeVec(n);
    FreeVec(l);
}

/* ------------------------------------------------------ default domain --- */

LONG bsd_GetDefaultDomainName(STRPTR buffer asm("a0"), LONG bufferSize asm("d0"),
                              struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s: bufferSize=%ld\n", __func__, bufferSize);
    struct SocketBase *root = SB_ROOT(base);

    if (buffer == NULL || bufferSize <= 0 || root->defaultDomain[0] == '\0')
        return 0; /* FALSE */

    strlcpy((char *)buffer, root->defaultDomain, (ULONG)bufferSize);
    return 1; /* TRUE */
}

VOID bsd_SetDefaultDomainName(STRPTR buffer asm("a0"), struct SocketBase *base asm("a6"))
{
    KprintfH("[bsdsocket] %s: name=%s\n", __func__, buffer != NULL ? (ULONG)buffer : (ULONG)"(null)");
    struct SocketBase *root = SB_ROOT(base);

    if (buffer == NULL)
    {
        root->defaultDomain[0] = '\0';
        return;
    }
    strlcpy(root->defaultDomain, (const char *)buffer, sizeof(root->defaultDomain));
}
