/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * socktest — exercise bsdsocket.library end to end from the app side:
 * OpenLibrary starts the stack (DHCP), gethostbyname resolves over lwIP DNS,
 * then a TCP connect/send/recv round trip.
 *
 *   socktest <host> <port> [request text]
 *   socktest example.com 80              (sends a HTTP/1.0 GET)
 *
 * Exercises: per-opener base, errno, DNS blocking, TCP connect/send/recv
 * blocking paths, orderly close. WaitSelect gets its own test later.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <exec/types.h>
#include <exec/libraries.h>

#include <proto/exec.h>

/* ---- minimal inline glue (NDK inline-header idiom, LVOs from the sfd) ---- */

static struct Library *SocketBase;

#define SB_JSR(offs) "jsr a6@(-" #offs ":W)"

static inline LONG sb_socket(LONG dom, LONG ty, LONG pr)
{
    register struct Library *a6 __asm("a6") = SocketBase;
    register LONG res __asm("d0");
    register LONG d0 __asm("d0") = dom;
    register LONG d1 __asm("d1") = ty;
    register LONG d2 __asm("d2") = pr;
    __asm volatile(SB_JSR(30) : "=r"(res) : "r"(a6), "0"(d0), "r"(d1), "r"(d2) : "a0", "a1", "cc", "memory");
    return res;
}

static inline LONG sb_connect(LONG s, void *name, LONG len)
{
    register struct Library *a6 __asm("a6") = SocketBase;
    register LONG res __asm("d0");
    register LONG d0 __asm("d0") = s;
    register void *a0 __asm("a0") = name;
    register LONG d1 __asm("d1") = len;
    __asm volatile(SB_JSR(54) : "=r"(res) : "r"(a6), "0"(d0), "r"(a0), "r"(d1) : "a1", "cc", "memory");
    return res;
}

static inline LONG sb_send(LONG s, void *buf, LONG len, LONG flags)
{
    register struct Library *a6 __asm("a6") = SocketBase;
    register LONG res __asm("d0");
    register LONG d0 __asm("d0") = s;
    register void *a0 __asm("a0") = buf;
    register LONG d1 __asm("d1") = len;
    register LONG d2 __asm("d2") = flags;
    __asm volatile(SB_JSR(66) : "=r"(res) : "r"(a6), "0"(d0), "r"(a0), "r"(d1), "r"(d2) : "a1", "cc", "memory");
    return res;
}

static inline LONG sb_recv(LONG s, void *buf, LONG len, LONG flags)
{
    register struct Library *a6 __asm("a6") = SocketBase;
    register LONG res __asm("d0");
    register LONG d0 __asm("d0") = s;
    register void *a0 __asm("a0") = buf;
    register LONG d1 __asm("d1") = len;
    register LONG d2 __asm("d2") = flags;
    __asm volatile(SB_JSR(78) : "=r"(res) : "r"(a6), "0"(d0), "r"(a0), "r"(d1), "r"(d2) : "a1", "cc", "memory");
    return res;
}

static inline LONG sb_CloseSocket(LONG s)
{
    register struct Library *a6 __asm("a6") = SocketBase;
    register LONG res __asm("d0");
    register LONG d0 __asm("d0") = s;
    __asm volatile(SB_JSR(120) : "=r"(res) : "r"(a6), "0"(d0) : "d1", "a0", "a1", "cc", "memory");
    return res;
}

static inline LONG sb_Errno(void)
{
    register struct Library *a6 __asm("a6") = SocketBase;
    register LONG res __asm("d0");
    __asm volatile(SB_JSR(162) : "=r"(res) : "r"(a6) : "d1", "a0", "a1", "cc", "memory");
    return res;
}

static inline ULONG sb_inet_addr(const char *cp)
{
    register struct Library *a6 __asm("a6") = SocketBase;
    register ULONG res __asm("d0");
    register const char *a0 __asm("a0") = cp;
    __asm volatile(SB_JSR(180) : "=r"(res) : "r"(a6), "r"(a0) : "d1", "a1", "cc", "memory");
    return res;
}

struct sb_hostent
{
    char *h_name;
    char **h_aliases;
    LONG h_addrtype;
    LONG h_length;
    char **h_addr_list;
};

static inline struct sb_hostent *sb_gethostbyname(const char *name)
{
    register struct Library *a6 __asm("a6") = SocketBase;
    register struct sb_hostent *res __asm("d0");
    register const char *a0 __asm("a0") = name;
    __asm volatile(SB_JSR(210) : "=r"(res) : "r"(a6), "r"(a0) : "d1", "a1", "cc", "memory");
    return res;
}

/* ---- the test ---- */

struct sb_sockaddr_in
{
    UBYTE sin_len;
    UBYTE sin_family;
    UWORD sin_port;
    ULONG sin_addr;
    UBYTE sin_zero[8];
};

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        printf("usage: socktest <host> <port> [text]\n");
        return 5;
    }

    SocketBase = OpenLibrary((CONST_STRPTR) "bsdsocket.library", 4);
    if (SocketBase == NULL)
    {
        printf("socktest: cannot open bsdsocket.library v4\n");
        return 20;
    }
    printf("bsdsocket.library %u.%u open (stack starting brings DHCP up)\n",
           SocketBase->lib_Version, SocketBase->lib_Revision);

    int rc = 10;
    ULONG addr = sb_inet_addr(argv[1]);
    if (addr == 0xFFFFFFFF)
    {
        printf("resolving %s...\n", argv[1]);
        struct sb_hostent *he = sb_gethostbyname(argv[1]);
        if (he == NULL || he->h_addr_list == NULL || he->h_addr_list[0] == NULL)
        {
            printf("socktest: cannot resolve %s\n", argv[1]);
            goto out;
        }
        addr = *(ULONG *)he->h_addr_list[0];
    }
    printf("address %lu.%lu.%lu.%lu\n",
           (unsigned long)(addr >> 24) & 0xFF, (unsigned long)(addr >> 16) & 0xFF,
           (unsigned long)(addr >> 8) & 0xFF, (unsigned long)addr & 0xFF);

    LONG s = sb_socket(2 /*AF_INET*/, 1 /*SOCK_STREAM*/, 0);
    if (s < 0)
    {
        printf("socktest: socket() errno %ld\n", (long)sb_Errno());
        goto out;
    }

    struct sb_sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_len = sizeof(sin);
    sin.sin_family = 2;
    sin.sin_port = (UWORD)atoi(argv[2]);
    sin.sin_addr = addr;

    printf("connecting...\n");
    if (sb_connect(s, &sin, sizeof(sin)) != 0)
    {
        printf("socktest: connect() errno %ld\n", (long)sb_Errno());
        sb_CloseSocket(s);
        goto out;
    }

    char req[512];
    if (argc > 3)
    {
        strncpy(req, argv[3], sizeof(req) - 3);
        req[sizeof(req) - 3] = '\0';
        strcat(req, "\r\n");
    }
    else
    {
        strcpy(req, "GET / HTTP/1.0\r\n\r\n");
    }

    LONG n = sb_send(s, req, (LONG)strlen(req), 0);
    printf("sent %ld bytes\n", (long)n);

    static char buf[2048];
    LONG total = 0;
    for (;;)
    {
        n = sb_recv(s, buf, sizeof(buf) - 1, 0);
        if (n < 0)
        {
            printf("socktest: recv() errno %ld\n", (long)sb_Errno());
            break;
        }
        if (n == 0)
            break; /* orderly EOF */
        buf[n] = '\0';
        if (total < 4096)
            printf("%s", buf);
        total += n;
    }
    printf("\n--- received %ld bytes total ---\n", (long)total);
    sb_CloseSocket(s);
    rc = 0;

out:
    CloseLibrary(SocketBase);
    return rc;
}
