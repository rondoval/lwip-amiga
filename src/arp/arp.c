/* SPDX-License-Identifier: BSD-4-Clause-UC */
/*
 * Copyright (c) 1984 Regents of the University of California.
 * All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Sun Microsystems, Inc.
 *
 * Redistribution and use in source and binary forms are permitted
 * provided that the above copyright notice and this paragraph are
 * duplicated in all such forms and that any documentation,
 * advertising materials, and other materials related to such
 * distribution and use acknowledge that the software was developed
 * by the University of California, Berkeley.  The name of the
 * University may not be used to endorse or promote products derived
 * from this software without specific prior written permission.
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
 * WARRANTIES OF MERCHANTIBILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 */

/*
 * Arp — display, set and delete the stack's ARP table entries.
 *
 * Derived from 4.3BSD arp(8) (contributed to Berkeley by Sun Microsystems),
 * restructured for AmigaOS ReadArgs with a Roadshow-compatible template
 * and lwip-amiga's SIOC*ARP IoctlSocket requests (net/if_arp_ioctl.h). The
 * whole-table listing uses SIOCGARPT instead of the original's /dev/kmem
 * walk. PUBLISH/PROXY entries are not supported by this stack, ever — the
 * template omits Roadshow's switches; a "pub" token in a FILE batch line is
 * still recognized and rejected with a clear per-line error.
 */

#include <stdio.h>
#include <string.h>

#include <dos/dos.h>
#include <dos/rdargs.h>
#include <exec/types.h>

#include <proto/dos.h>
#include <proto/exec.h>

#include <sys/errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <net/if_arp.h>
#include <net/if_arp_ioctl.h>

#include <proto/socket.h> /* bsdsocket.library inline glue */

struct Library *SocketBase; /* the bsdsocket inline glue in <proto/socket.h> */

/* the library mirrors this layout and precomputed request values (sb_arp.c /
 * sb_base.h); catch NDK packing surprises (m68k only: host IntelliSense sees
 * 64-bit __LONG) */
#ifndef __INTELLISENSE__
_Static_assert(sizeof(struct arpreq) == 36, "arpreq wire ABI");
_Static_assert(SIOCSARP == 0x8024691EUL, "SB_SIOCSARP mirror");
_Static_assert(SIOCDARP == 0x80246920UL, "SB_SIOCDARP mirror");
_Static_assert(SIOCGARP == 0xC0246926UL, "SB_SIOCGARP mirror");
_Static_assert(SIOCGARPT == 0xC00C695BUL, "SB_SIOCGARPT mirror");
#endif

#define ARG_TEMPLATE "-a=ALL/S,-d=DELETE/S,-s=SET/S,HOSTNAME,ADDRESS,TEMP/S," \
                     "-f=FILE/K,-n=NONAMES/S=NUMBERS/S"
enum
{
    ARG_ALL,
    ARG_DELETE,
    ARG_SET,
    ARG_HOSTNAME,
    ARG_ADDRESS,
    ARG_TEMP,
    ARG_FILE,
    ARG_NONAMES,
    ARG_COUNT
};

#define FILE_TEMPLATE "HOSTNAME/A,ADDRESS/A,TEMP/S,PUB=PUBLISH/S"
enum
{
    FARG_HOSTNAME,
    FARG_ADDRESS,
    FARG_TEMP,
    FARG_PUB,
    FARG_COUNT
};

static int arp_socket = -1;

static const char *arp_strerror(LONG err)
{
    switch (err)
    {
    case ENXIO:
        return "no such entry";
    case EADDRINUSE:
        return "a permanent entry exists - delete it first";
    case ENETUNREACH:
        return "no interface with a route to that host";
    case ENOBUFS:
        return "the ARP table is full";
    case EINVAL:
        return "invalid address";
    default:
        return "request failed";
    }
}

/* dotted quad or resolver name -> network-order IPv4 address */
static int resolve_host(const char *name, ULONG *addr)
{
    in_addr_t a = inet_addr((STRPTR)name);
    if (a != INADDR_NONE)
    {
        *addr = a;
        return 1;
    }
    struct hostent *he = gethostbyname((STRPTR)name);
    if (he == NULL || he->h_addr == NULL)
    {
        fprintf(stderr, "Arp: %s: unknown host\n", name);
        return 0;
    }
    memcpy(addr, he->h_addr, sizeof(*addr));
    return 1;
}

/* six hex bytes separated by colons, e.g. 00:30:ab:0e:d5:ee */
static int parse_ether(const char *s, UBYTE mac[6])
{
    unsigned int b[6];
    if (sscanf(s, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6)
        return 0;
    for (int i = 0; i < 6; i++)
    {
        if (b[i] > 0xff)
            return 0;
        mac[i] = (UBYTE)b[i];
    }
    return 1;
}

static void fill_pa(struct arpreq *ar, ULONG addr)
{
    memset(ar, 0, sizeof(*ar));
    struct sockaddr_in *sin = (struct sockaddr_in *)&ar->arp_pa;
    sin->sin_len = sizeof(*sin);
    sin->sin_family = AF_INET;
    sin->sin_addr.s_addr = addr;
    ar->arp_ha.sa_len = sizeof(ar->arp_ha);
}

static void print_entry(const struct arpreq *ar, int numeric)
{
    const struct sockaddr_in *sin = (const struct sockaddr_in *)&ar->arp_pa;
    ULONG a = sin->sin_addr.s_addr;

    const char *name = NULL;
    if (!numeric)
    {
        struct hostent *he = gethostbyaddr((STRPTR)&sin->sin_addr, sizeof(sin->sin_addr), AF_INET);
        if (he != NULL)
            name = (const char *)he->h_name;
    }
    printf("%s (", name != NULL ? name : "?");
    printf("%lu.%lu.%lu.%lu) at ",
           (a >> 24) & 0xff, (a >> 16) & 0xff, (a >> 8) & 0xff, a & 0xff);

    if (ar->arp_flags & ATF_COM)
    {
        const UBYTE *e = (const UBYTE *)ar->arp_ha.sa_data;
        printf("%02lx:%02lx:%02lx:%02lx:%02lx:%02lx",
               (ULONG)e[0], (ULONG)e[1], (ULONG)e[2], (ULONG)e[3], (ULONG)e[4], (ULONG)e[5]);
    }
    else
        printf("(incomplete)");

    if (ar->arp_flags & ATF_PERM)
        printf(" permanent");
    printf("\n");
}

/* display the entry for one host */
static int do_get(const char *host, int numeric)
{
    ULONG addr;
    if (!resolve_host(host, &addr))
        return RETURN_ERROR;

    struct arpreq ar;
    fill_pa(&ar, addr);
    if (IoctlSocket(arp_socket, SIOCGARP, &ar) != 0)
    {
        ULONG a = addr;
        printf("%s (%lu.%lu.%lu.%lu) -- no entry\n", host,
               (a >> 24) & 0xff, (a >> 16) & 0xff, (a >> 8) & 0xff, a & 0xff);
        return RETURN_WARN;
    }
    print_entry(&ar, numeric);
    return RETURN_OK;
}

/* display the whole table */
static int do_dump(int numeric)
{
    struct arptabreq atr = {0, 0, NULL};
    if (IoctlSocket(arp_socket, SIOCGARPT, &atr) != 0)
    {
        fprintf(stderr, "Arp: %s\n", arp_strerror(Errno()));
        return RETURN_ERROR;
    }
    if (atr.atr_inuse == 0)
    {
        printf("the ARP table is empty\n");
        return RETURN_OK;
    }

    /* small slack: entries may appear between the size query and the dump */
    LONG cap = atr.atr_inuse + 4;
    struct arpreq *tab = AllocVec((ULONG)cap * sizeof(*tab), MEMF_ANY | MEMF_CLEAR);
    if (tab == NULL)
        return RETURN_FAIL;

    atr.atr_size = cap;
    atr.atr_table = tab;
    if (IoctlSocket(arp_socket, SIOCGARPT, &atr) != 0)
    {
        fprintf(stderr, "Arp: %s\n", arp_strerror(Errno()));
        FreeVec(tab);
        return RETURN_ERROR;
    }
    for (LONG i = 0; i < atr.atr_size; i++)
        print_entry(&tab[i], numeric);

    FreeVec(tab);
    return RETURN_OK;
}

static int do_set(const char *host, const char *ether, int temp)
{
    ULONG addr;
    if (!resolve_host(host, &addr))
        return RETURN_ERROR;

    struct arpreq ar;
    fill_pa(&ar, addr);
    if (!parse_ether(ether, (UBYTE *)ar.arp_ha.sa_data))
    {
        fprintf(stderr, "Arp: %s: invalid Ethernet address (need aa:bb:cc:dd:ee:ff)\n", ether);
        return RETURN_ERROR;
    }
    ar.arp_flags = temp ? 0 : ATF_PERM;

    if (IoctlSocket(arp_socket, SIOCSARP, &ar) != 0)
    {
        fprintf(stderr, "Arp: %s: %s\n", host, arp_strerror(Errno()));
        return RETURN_ERROR;
    }
    return RETURN_OK;
}

static int do_delete(const char *host)
{
    ULONG addr;
    if (!resolve_host(host, &addr))
        return RETURN_ERROR;

    struct arpreq ar;
    fill_pa(&ar, addr);
    if (IoctlSocket(arp_socket, SIOCDARP, &ar) != 0)
    {
        fprintf(stderr, "Arp: %s: %s\n", host, arp_strerror(Errno()));
        return RETURN_ERROR;
    }
    ULONG a = addr;
    printf("%s (%lu.%lu.%lu.%lu) deleted\n", host,
           (a >> 24) & 0xff, (a >> 16) & 0xff, (a >> 8) & 0xff, a & 0xff);
    return RETURN_OK;
}

/* batch load: one "hostname ether_addr [temp] [pub]" entry per line */
static int do_file(const char *fname)
{
    FILE *f = fopen(fname, "r");
    if (f == NULL)
    {
        fprintf(stderr, "Arp: cannot open %s\n", fname);
        return RETURN_ERROR;
    }

    int rc = RETURN_OK;
    int lineno = 0;
    char line[258]; /* two spare bytes: a missing final newline is appended */
    while (fgets(line, sizeof(line) - 2, f) != NULL)
    {
        lineno++;
        /* skip blank and comment lines; ReadArgs needs the newline kept */
        char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '\n' || *p == '\0' || *p == '#')
            continue;
        size_t len = strlen(p);
        if (p[len - 1] != '\n')
        {
            p[len] = '\n'; /* last line without newline; line[] has room */
            p[len + 1] = '\0';
            len++;
        }

        struct RDArgs *src = AllocDosObject(DOS_RDARGS, NULL);
        if (src == NULL)
        {
            rc = RETURN_FAIL;
            break;
        }
        src->RDA_Source.CS_Buffer = (STRPTR)p;
        src->RDA_Source.CS_Length = (LONG)len;
        src->RDA_Source.CS_CurChr = 0;
        src->RDA_Flags |= RDAF_NOPROMPT;

        LONG fargs[FARG_COUNT];
        memset(fargs, 0, sizeof(fargs));
        struct RDArgs *rda = ReadArgs((CONST_STRPTR)FILE_TEMPLATE, fargs, src);
        if (rda == NULL)
        {
            fprintf(stderr, "Arp: %s line %d: bad entry\n", fname, lineno);
            rc = RETURN_ERROR;
        }
        else if (fargs[FARG_PUB] != 0)
        {
            fprintf(stderr, "Arp: %s line %d: published entries are not supported "
                            "by this stack\n", fname, lineno);
            rc = RETURN_ERROR;
        }
        else
        {
            int r = do_set((const char *)fargs[FARG_HOSTNAME],
                           (const char *)fargs[FARG_ADDRESS], fargs[FARG_TEMP] != 0);
            if (r > rc)
                rc = r;
        }
        if (rda != NULL)
            FreeArgs(rda);
        FreeDosObject(DOS_RDARGS, src);
    }

    fclose(f);
    return rc;
}

int main(void)
{
    LONG args[ARG_COUNT];
    memset(args, 0, sizeof(args));
    struct RDArgs *rda = ReadArgs((CONST_STRPTR)ARG_TEMPLATE, args, NULL);
    if (rda == NULL)
    {
        PrintFault(IoErr(), (CONST_STRPTR) "Arp");
        return RETURN_ERROR;
    }

    int rc;
    const char *host = (const char *)args[ARG_HOSTNAME];
    const char *addr = (const char *)args[ARG_ADDRESS];

    if (args[ARG_SET] != 0 && (host == NULL || addr == NULL))
    {
        fprintf(stderr, "Arp: SET needs HOSTNAME and ADDRESS\n");
        rc = RETURN_ERROR;
        goto out_args;
    }
    if (args[ARG_DELETE] != 0 && host == NULL)
    {
        fprintf(stderr, "Arp: DELETE needs HOSTNAME\n");
        rc = RETURN_ERROR;
        goto out_args;
    }
    if (args[ARG_SET] == 0 && args[ARG_DELETE] == 0 && args[ARG_ALL] == 0 &&
        args[ARG_FILE] == 0 && host == NULL)
    {
        fprintf(stderr, "Usage: arp %s\n", ARG_TEMPLATE);
        rc = RETURN_ERROR;
        goto out_args;
    }

    SocketBase = OpenLibrary((CONST_STRPTR) "bsdsocket.library", 4);
    if (SocketBase == NULL)
    {
        fprintf(stderr, "Arp: cannot open bsdsocket.library\n");
        rc = RETURN_FAIL;
        goto out_args;
    }
    arp_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (arp_socket < 0)
    {
        fprintf(stderr, "Arp: cannot create socket\n");
        rc = RETURN_FAIL;
        goto out_lib;
    }

    int numeric = args[ARG_NONAMES] != 0;
    if (args[ARG_SET] != 0)
        rc = do_set(host, addr, args[ARG_TEMP] != 0);
    else if (args[ARG_DELETE] != 0)
        rc = do_delete(host);
    else if (args[ARG_FILE] != 0)
        rc = do_file((const char *)args[ARG_FILE]);
    else if (args[ARG_ALL] != 0)
        rc = do_dump(numeric);
    else
        rc = do_get(host, numeric);

    CloseSocket(arp_socket);
out_lib:
    CloseLibrary(SocketBase);
out_args:
    FreeArgs(rda);
    return rc;
}
