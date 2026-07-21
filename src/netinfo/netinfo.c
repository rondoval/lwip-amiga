/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * netinfo — read-only interface status for bsdsocket.library (ifconfig-like).
 *
 * Enumerates interfaces via the Roadshow interface-query API
 * (ObtainInterfaceList / QueryInterfaceTagList) and prints each one's
 * address, netmask, broadcast, MTU, MAC, link state, address source
 * (DHCP/static) and DNS servers. It never configures anything — the stack
 * is configured from ENVARC:netstack.prefs. (Gateway/routes are not part
 * of the interface-query API and are not shown, as with classic ifconfig.)
 *
 * A bsdsocket.library client, so it uses the NDK bsdsocket headers and
 * their inline glue rather than open-coding LVO stubs.
 */

#include <stdio.h>
#include <string.h>

#include <exec/types.h>
#include <exec/libraries.h>
#include <exec/lists.h>

#include <utility/tagitem.h>
#include <libraries/bsdsocket.h> /* IFQ_*, SM_*, IFABT_* */

#include <netinet/in.h> /* struct sockaddr_in */

#include <proto/exec.h>
#include <proto/socket.h> /* bsdsocket.library inline glue */

struct Library *SocketBase; /* the bsdsocket inline glue in <proto/socket.h> */

static void print_addr(const char *label, const struct sockaddr_in *sa)
{
    unsigned long a = sa->sin_addr.s_addr; /* network order == host order on 68k */
    printf("%s %lu.%lu.%lu.%lu", label,
           (a >> 24) & 0xff, (a >> 16) & 0xff, (a >> 8) & 0xff, a & 0xff);
}

static void print_interface(STRPTR name)
{
    struct sockaddr_in addr, mask, bcast, dns0, dns1;
    UBYTE hw[16];
    LONG mtu = 0, state = SM_Down, bindtype = IFABT_Unknown, unit = 0;
    STRPTR devname = NULL;

    memset(&addr, 0, sizeof(addr));
    memset(&mask, 0, sizeof(mask));
    memset(&bcast, 0, sizeof(bcast));
    memset(&dns0, 0, sizeof(dns0));
    memset(&dns1, 0, sizeof(dns1));
    memset(hw, 0, sizeof(hw));

    QueryInterfaceTags(name,
                       IFQ_Address, (Tag)&addr,
                       IFQ_NetMask, (Tag)&mask,
                       IFQ_BroadcastAddress, (Tag)&bcast,
                       IFQ_MTU, (Tag)&mtu,
                       IFQ_State, (Tag)&state,
                       IFQ_AddressBindType, (Tag)&bindtype,
                       IFQ_HardwareAddress, (Tag)hw,
                       IFQ_DeviceName, (Tag)&devname,
                       IFQ_DeviceUnit, (Tag)&unit,
                       IFQ_PrimaryDNSAddress, (Tag)&dns0,
                       IFQ_SecondaryDNSAddress, (Tag)&dns1,
                       TAG_END);

    printf("%s\t%s  (%s unit %ld)\n", (char *)name,
           state == SM_Up ? "UP" : "DOWN",
           devname != NULL ? (char *)devname : "?", (long)unit);

    printf("\t");
    print_addr("inet", &addr);
    print_addr("  netmask", &mask);
    print_addr("  broadcast", &bcast);
    printf("\n");

    printf("\tether %02lx:%02lx:%02lx:%02lx:%02lx:%02lx  mtu %ld  %s\n",
           (unsigned long)hw[0], (unsigned long)hw[1], (unsigned long)hw[2],
           (unsigned long)hw[3], (unsigned long)hw[4], (unsigned long)hw[5],
           (long)mtu, bindtype == IFABT_Dynamic ? "DHCP" : "static");

    if (dns0.sin_addr.s_addr != 0 || dns1.sin_addr.s_addr != 0)
    {
        printf("\t");
        if (dns0.sin_addr.s_addr != 0)
            print_addr("dns", &dns0);
        if (dns1.sin_addr.s_addr != 0)
            print_addr(dns0.sin_addr.s_addr != 0 ? " " : "dns", &dns1);
        printf("\n");
    }
}

int main(void)
{
    SocketBase = OpenLibrary((CONST_STRPTR) "bsdsocket.library", 4);
    if (SocketBase == NULL)
    {
        fprintf(stderr, "netinfo: cannot open bsdsocket.library\n");
        return 20;
    }

    char host[128];
    if (gethostname((STRPTR)host, sizeof(host)) == 0)
        printf("hostname %s\n", host);

    struct List *list = ObtainInterfaceList();
    if (list == NULL)
    {
        printf("no interfaces\n");
        CloseLibrary(SocketBase);
        return 0;
    }

    for (struct Node *n = list->lh_Head; n->ln_Succ != NULL; n = n->ln_Succ)
        print_interface((STRPTR)n->ln_Name);

    ReleaseInterfaceList(list);
    CloseLibrary(SocketBase);
    return 0;
}
