/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * bsdsocket.library — internal design.
 *
 * BASES. The classic per-opener child-base pattern: every OpenLibrary()
 * returns a fresh copy of the root base (jump table included), so each
 * opener — by convention each task — carries its own errno, fd table,
 * wait signal and timer. The root base (made by MakeLibrary) owns the
 * global state: the netstack singleton, the stack task, the netdev
 * attachment and the socket memory pool. SB_ROOT(base) reaches the root
 * from either kind.
 *
 * EXECUTION MODEL. Every API call takes the netstack core lock, works on
 * lwIP raw-API pcbs, and releases it on return. lwIP callbacks (RX from
 * the driver task, timers from the stack task, DNS) already run under
 * that lock. Blocking is Exec-native: a per-opener signal bit; callbacks
 * that change a socket's readiness Signal() the owning task. The wait
 * pattern is race-free because state only changes under the lock we hold:
 *     while (!cond) { SetSignal(0, sig); unlock; Wait(sig|break); lock; }
 *
 * MEMORY. Socket/datagram/pcb-side objects come from the root's Exec pool,
 * allocated and freed strictly under the core lock (the pool needs no
 * locking of its own). Packet payloads never appear here — recv copies
 * out of driver-owned pbufs (the one copy), send copies into DMA-backed
 * PBUF_RAM via tcp_write/COPY.
 */

#ifndef SB_BASE_H
#define SB_BASE_H

#include <exec/libraries.h>

#define __NOLIBBASE__
#define EXEC_BASE_NAME (*(struct ExecBase **)4UL)

#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#else
#include <proto/exec.h>
#endif

#include <exec/semaphores.h>
#include <exec/types.h>
#include <devices/timer.h>

#include <lwip/err.h>
#include <lwip/ip_addr.h>
#include <lwip/pbuf.h>

#include <netdev_if.h>

struct tcp_pcb;
struct udp_pcb;
struct raw_pcb;

/* --- wire-format structures (the app ABI; 4.4BSD with length bytes) ------ */

struct sb_sockaddr_in
{
    UBYTE sin_len;
    UBYTE sin_family;
    UWORD sin_port;    /* network order == native */
    ULONG sin_addr;    /* network order == native */
    UBYTE sin_zero[8];
};

struct sb_hostent
{
    char *h_name;
    char **h_aliases;
    LONG h_addrtype;
    LONG h_length;
    char **h_addr_list;
};

struct sb_protoent
{
    char *p_name;
    char **p_aliases;
    LONG p_proto;
};

struct sb_servent
{
    char *s_name;
    char **s_aliases;
    LONG s_port; /* network order (== native) */
    char *s_proto;
};

struct sb_msghdr
{
    APTR msg_name;
    ULONG msg_namelen;
    struct sb_iovec *msg_iov;
    ULONG msg_iovlen;
    APTR msg_control;
    ULONG msg_controllen;
    LONG msg_flags;
};

struct sb_iovec
{
    APTR iov_base;
    ULONG iov_len;
};

/* Roadshow DNS-list node (libraries/bsdsocket.h) */
struct sb_DomainNameServerNode
{
    struct MinNode dnsn_MinNode;
    LONG dnsn_Size;
    STRPTR dnsn_Address;
    LONG dnsn_UseCount;
};

/* a socket parked by ReleaseSocket/ReleaseCopyOfSocket */
struct SbReleased
{
    struct MinNode node;
    LONG id;
    struct SbSocket *s;
};

struct sb_timeval
{
    ULONG tv_secs;
    ULONG tv_micro;
};

#define SB_AF_INET 2
#define SB_SOCK_STREAM 1
#define SB_SOCK_DGRAM 2
#define SB_SOCK_RAW 3
#define SB_IPPROTO_ICMP 1
#define SB_IPPROTO_TCP 6
#define SB_IPPROTO_UDP 17

#define SB_SOL_SOCKET 0xFFFF
#define SB_SO_REUSEADDR 0x0004
#define SB_SO_KEEPALIVE 0x0008
#define SB_SO_BROADCAST 0x0020
#define SB_SO_LINGER 0x0080
#define SB_SO_SNDBUF 0x1001
#define SB_SO_RCVBUF 0x1002
#define SB_SO_ERROR 0x1007
#define SB_SO_TYPE 0x1008
#define SB_TCP_NODELAY 1

#define SB_MSG_OOB 0x01
#define SB_MSG_PEEK 0x02
#define SB_MSG_WAITALL 0x40
#define SB_MSG_DONTWAIT 0x80

#define SB_FIONBIO 0x8004667EUL
#define SB_FIONREAD 0x4004667FUL

/* BSD errno values (netinclude/sys/errno.h) */
#define SB_EINTR 4
#define SB_EBADF 9
#define SB_ENOMEM 12
#define SB_EACCES 13
#define SB_EFAULT 14
#define SB_EINVAL 22
#define SB_EMFILE 24
#define SB_EPIPE 32
#define SB_EWOULDBLOCK 35
#define SB_EINPROGRESS 36
#define SB_EALREADY 37
#define SB_ENOTSOCK 38
#define SB_EDESTADDRREQ 39
#define SB_EMSGSIZE 40
#define SB_ENOPROTOOPT 42
#define SB_EPROTONOSUPPORT 43
#define SB_ESOCKTNOSUPPORT 44
#define SB_EOPNOTSUPP 45
#define SB_EAFNOSUPPORT 47
#define SB_EADDRINUSE 48
#define SB_EADDRNOTAVAIL 49
#define SB_ENETUNREACH 51
#define SB_ECONNABORTED 53
#define SB_ECONNRESET 54
#define SB_ENOBUFS 55
#define SB_EISCONN 56
#define SB_ENOTCONN 57
#define SB_ESHUTDOWN 58
#define SB_ETIMEDOUT 60
#define SB_ECONNREFUSED 61
#define SB_EHOSTUNREACH 65

#define SB_HOST_NOT_FOUND 1
#define SB_TRY_AGAIN 2

/* SocketBaseTagList encoding (netinclude/libraries/bsdsocket.h) */
#define SBTF_REF 0x8000UL
#define SBTF_SET 1UL
#define SBTM_CODE(td) (((td) >> 1) & 0x3FFF)
#define SBTC_BREAKMASK 1
#define SBTC_SIGIOMASK 2
#define SBTC_SIGURGMASK 3
#define SBTC_SIGEVENTMASK 4
#define SBTC_ERRNO 6
#define SBTC_HERRNO 7
#define SBTC_DTABLESIZE 8
#define SBTC_ERRNOBYTEPTR 21
#define SBTC_ERRNOWORDPTR 22
#define SBTC_ERRNOLONGPTR 24
#define SBTC_HERRNOLONGPTR 25

/* --- sockets -------------------------------------------------------------- */

#define SB_FD_COUNT 64
#define SB_DGRAM_QMAX 32
#define SB_ACCEPT_QMAX 16
#define SB_HOSTNAME_MAX 128

typedef enum
{
    SBT_TCP = 1,
    SBT_UDP = 2,
    SBT_RAW = 3
} SbSockType;

struct SbDgram
{
    struct MinNode node;
    struct pbuf *p;
    ULONG addr; /* network order */
    UWORD port;
    UWORD pad;
};

struct SbSocket
{
    struct MinNode node; /* accept-queue linkage */
    struct SocketBase *owner;    /* the base whose task gets woken; NULL while
                                  * parked by ReleaseSocket */
    struct SocketBase *rootBase; /* never changes; owns the pool */
    UBYTE refs;                  /* fd references (Dup2Socket, ReleaseCopyOf) */

    UBYTE type;      /* SbSockType */
    UBYTE nonblock;
    UBYTE connected; /* TCP */
    UBYTE connecting;
    UBYTE listening;
    UBYTE rxeof;
    UBYTE shut_rd;
    UBYTE shut_wr;

    union
    {
        struct tcp_pcb *tcp;
        struct udp_pcb *udp;
        struct raw_pcb *raw;
        APTR any;
    } pcb;

    LONG err; /* pending errno; pcb.any == NULL after fatal errors */

    /* TCP receive: pbuf chain, consumed via pbuf_free_header */
    struct pbuf *rxq;

    /* UDP/RAW receive: queued datagrams */
    struct MinList dgrams;
    ULONG ndgrams;

    /* listener: pre-created sockets awaiting accept() */
    struct MinList acceptq;
    ULONG naccept;
};

/* --- the library base ------------------------------------------------------ */

struct SocketBase
{
    struct Library libNode;
    ULONG segList;
    struct SocketBase *root; /* NULL in the root base itself */

    /* --- root-only state --- */
    struct SignalSemaphore openLock; /* guards stack startup/shutdown */
    struct Task *stackTask;
    APTR sockPool;      /* objects; alloc/free under the core lock only */
    ULONG openCount;    /* child bases alive */
    struct MinList releasedSockets; /* ReleaseSocket parking lot (core lock) */
    LONG nextSockId;
    char defaultDomain[SB_HOSTNAME_MAX];

    /* --- per-opener state (child bases; garbage in the root) --- */
    struct Task *task;
    BYTE sigBit;
    UBYTE pad0[3];
    ULONG breakMask;
    ULONG sigIoMask;
    ULONG sigUrgMask;

    LONG internalErrno;
    APTR errnoPtr;   /* defaults to &internalErrno */
    ULONG errnoSize; /* 1, 2 or 4 */
    LONG hErrno;
    LONG *hErrnoPtr; /* defaults to &hErrno */

    struct SbSocket **fd; /* SB_FD_COUNT entries */

    struct MsgPort *timerPort; /* WaitSelect timeouts */
    struct timerequest *timerReq;
    UBYTE timerOpen;
    UBYTE dnsDone;
    UBYTE pad1[2];

    /* DNS + netdb per-opener result storage */
    ip_addr_t dnsAddr;
    LONG dnsErr;
    struct sb_hostent host;
    char hostName[SB_HOSTNAME_MAX];
    ULONG hostAddr;        /* network order */
    char *hostAddrList[2];
    char *hostAliases[1];
    struct sb_protoent proto;
    char *protoAliases[1];
    struct sb_servent serv;
    char *servAliases[1];
    ULONG protoIdx; /* get*ent iterators */
    ULONG servIdx;
    char ntoaBuf[20];
};

#define SB_ROOT(b) ((b)->root != NULL ? (b)->root : (b))

/* --- internals ------------------------------------------------------------- */

/* errno plumbing (sb_misc.c) */
void sb_set_errno(struct SocketBase *base, LONG code);
void sb_set_herrno(struct SocketBase *base, LONG code);

/* socket core (sb_socket.c) */
struct SbSocket *sb_sock_alloc(struct SocketBase *base, SbSockType type);
void sb_sock_free(struct SocketBase *base, struct SbSocket *s); /* under lock */
LONG sb_fd_alloc(struct SocketBase *base, struct SbSocket *s);
struct SbSocket *sb_fd_get(struct SocketBase *base, LONG fd);
void sb_wake(struct SbSocket *s);
LONG sb_wait(struct SocketBase *base); /* 0 or SB_EINTR; drops+retakes the lock */
void sb_tcp_wire(struct SbSocket *s);  /* attach lwIP callbacks to s->pcb.tcp */
err_t sb_tcp_connected_cb(void *arg, struct tcp_pcb *tpcb, err_t err);
void sb_listen_wire(struct SbSocket *s);
void sb_udp_wire(struct SbSocket *s);
void sb_raw_wire(struct SbSocket *s);
BOOL sb_sock_readable(const struct SbSocket *s);
BOOL sb_sock_writable(struct SbSocket *s);
LONG sb_map_err(signed char lwip_err);

/* library plumbing (main.c) */
extern const APTR bsdsocket_functable[];
struct SocketBase *LibOpen(ULONG version asm("d0"), struct SocketBase *base asm("a6"));
ULONG LibClose(struct SocketBase *base asm("a6"));
ULONG LibExpunge(struct SocketBase *base asm("a6"));
ULONG LibNull(void);
LONG LibStub(void);
APTR LibStubNull(void); /* for the pointer-returning unimplemented LVOs */

/* stack task (sb_stack.c) */
LONG sb_stack_start(struct SocketBase *root); /* under root->openLock */
void sb_stack_stop(struct SocketBase *root);

/* --- the implemented API surface (register conventions from the NDK sfd) --- */

struct TagItem;

LONG bsd_socket(LONG domain asm("d0"), LONG type asm("d1"), LONG protocol asm("d2"), struct SocketBase *base asm("a6"));
LONG bsd_bind(LONG sock asm("d0"), APTR name asm("a0"), LONG namelen asm("d1"), struct SocketBase *base asm("a6"));
LONG bsd_listen(LONG sock asm("d0"), LONG backlog asm("d1"), struct SocketBase *base asm("a6"));
LONG bsd_accept(LONG sock asm("d0"), APTR addr asm("a0"), APTR addrlen asm("a1"), struct SocketBase *base asm("a6"));
LONG bsd_connect(LONG sock asm("d0"), APTR name asm("a0"), LONG namelen asm("d1"), struct SocketBase *base asm("a6"));
LONG bsd_sendto(LONG sock asm("d0"), APTR buf asm("a0"), LONG len asm("d1"), LONG flags asm("d2"), APTR to asm("a1"), LONG tolen asm("d3"), struct SocketBase *base asm("a6"));
LONG bsd_send(LONG sock asm("d0"), APTR buf asm("a0"), LONG len asm("d1"), LONG flags asm("d2"), struct SocketBase *base asm("a6"));
LONG bsd_recvfrom(LONG sock asm("d0"), APTR buf asm("a0"), LONG len asm("d1"), LONG flags asm("d2"), APTR addr asm("a1"), APTR addrlen asm("a2"), struct SocketBase *base asm("a6"));
LONG bsd_recv(LONG sock asm("d0"), APTR buf asm("a0"), LONG len asm("d1"), LONG flags asm("d2"), struct SocketBase *base asm("a6"));
LONG bsd_shutdown(LONG sock asm("d0"), LONG how asm("d1"), struct SocketBase *base asm("a6"));
LONG bsd_setsockopt(LONG sock asm("d0"), LONG level asm("d1"), LONG optname asm("d2"), APTR optval asm("a0"), LONG optlen asm("d3"), struct SocketBase *base asm("a6"));
LONG bsd_getsockopt(LONG sock asm("d0"), LONG level asm("d1"), LONG optname asm("d2"), APTR optval asm("a0"), APTR optlen asm("a1"), struct SocketBase *base asm("a6"));
LONG bsd_getsockname(LONG sock asm("d0"), APTR name asm("a0"), APTR namelen asm("a1"), struct SocketBase *base asm("a6"));
LONG bsd_getpeername(LONG sock asm("d0"), APTR name asm("a0"), APTR namelen asm("a1"), struct SocketBase *base asm("a6"));
LONG bsd_IoctlSocket(LONG sock asm("d0"), ULONG req asm("d1"), APTR argp asm("a0"), struct SocketBase *base asm("a6"));
LONG bsd_CloseSocket(LONG sock asm("d0"), struct SocketBase *base asm("a6"));
LONG bsd_WaitSelect(LONG nfds asm("d0"), APTR readfds asm("a0"), APTR writefds asm("a1"), APTR exceptfds asm("a2"), APTR timeout asm("a3"), ULONG *signals asm("d1"), struct SocketBase *base asm("a6"));
VOID bsd_SetSocketSignals(ULONG intMask asm("d0"), ULONG ioMask asm("d1"), ULONG urgMask asm("d2"), struct SocketBase *base asm("a6"));
LONG bsd_getdtablesize(struct SocketBase *base asm("a6"));
LONG bsd_Errno(struct SocketBase *base asm("a6"));
VOID bsd_SetErrnoPtr(APTR errnoPtr asm("a0"), LONG size asm("d0"), struct SocketBase *base asm("a6"));
STRPTR bsd_Inet_NtoA(ULONG ip asm("d0"), struct SocketBase *base asm("a6"));
ULONG bsd_inet_addr(STRPTR cp asm("a0"), struct SocketBase *base asm("a6"));
ULONG bsd_Inet_LnaOf(ULONG in asm("d0"), struct SocketBase *base asm("a6"));
ULONG bsd_Inet_NetOf(ULONG in asm("d0"), struct SocketBase *base asm("a6"));
ULONG bsd_Inet_MakeAddr(ULONG net asm("d0"), ULONG host asm("d1"), struct SocketBase *base asm("a6"));
ULONG bsd_inet_network(STRPTR cp asm("a0"), struct SocketBase *base asm("a6"));
APTR bsd_gethostbyname(STRPTR name asm("a0"), struct SocketBase *base asm("a6"));
APTR bsd_gethostbyaddr(STRPTR addr asm("a0"), LONG len asm("d0"), LONG type asm("d1"), struct SocketBase *base asm("a6"));
LONG bsd_gethostname(STRPTR name asm("a0"), LONG namelen asm("d0"), struct SocketBase *base asm("a6"));
ULONG bsd_gethostid(struct SocketBase *base asm("a6"));
LONG bsd_SocketBaseTagList(struct TagItem *tags asm("a0"), struct SocketBase *base asm("a6"));
LONG bsd_inet_aton(STRPTR cp asm("a0"), APTR addr asm("a1"), struct SocketBase *base asm("a6"));
STRPTR bsd_inet_ntop(LONG af asm("d0"), APTR src asm("a0"), STRPTR dst asm("a1"), LONG size asm("d1"), struct SocketBase *base asm("a6"));
LONG bsd_inet_pton(LONG af asm("d0"), STRPTR src asm("a0"), APTR dst asm("a1"), struct SocketBase *base asm("a6"));
APTR bsd_getprotobyname(STRPTR name asm("a0"), struct SocketBase *base asm("a6"));
APTR bsd_getprotobynumber(LONG proto asm("d0"), struct SocketBase *base asm("a6"));

/* sb_misc2.c */
LONG bsd_Dup2Socket(LONG oldSock asm("d0"), LONG newSock asm("d1"), struct SocketBase *base asm("a6"));
LONG bsd_sendmsg(LONG sock asm("d0"), APTR msg asm("a0"), LONG flags asm("d1"), struct SocketBase *base asm("a6"));
LONG bsd_recvmsg(LONG sock asm("d0"), APTR msg asm("a0"), LONG flags asm("d1"), struct SocketBase *base asm("a6"));
VOID bsd_vsyslog(LONG pri asm("d0"), STRPTR msg asm("a0"), APTR args asm("a1"), struct SocketBase *base asm("a6"));
LONG bsd_ObtainSocket(LONG id asm("d0"), LONG domain asm("d1"), LONG type asm("d2"), LONG protocol asm("d3"), struct SocketBase *base asm("a6"));
LONG bsd_ReleaseSocket(LONG sock asm("d0"), LONG id asm("d1"), struct SocketBase *base asm("a6"));
LONG bsd_ReleaseCopyOfSocket(LONG sock asm("d0"), LONG id asm("d1"), struct SocketBase *base asm("a6"));
APTR bsd_getservbyname(STRPTR name asm("a0"), STRPTR proto asm("a1"), struct SocketBase *base asm("a6"));
APTR bsd_getservbyport(LONG port asm("d0"), STRPTR proto asm("a0"), struct SocketBase *base asm("a6"));
APTR bsd_getnetbyname(STRPTR name asm("a0"), struct SocketBase *base asm("a6"));
APTR bsd_getnetbyaddr(ULONG net asm("d0"), LONG type asm("d1"), struct SocketBase *base asm("a6"));
VOID bsd_setnetent(LONG stayOpen asm("d0"), struct SocketBase *base asm("a6"));
VOID bsd_endnetent(struct SocketBase *base asm("a6"));
APTR bsd_getnetent(struct SocketBase *base asm("a6"));
VOID bsd_setprotoent(LONG stayOpen asm("d0"), struct SocketBase *base asm("a6"));
VOID bsd_endprotoent(struct SocketBase *base asm("a6"));
APTR bsd_getprotoent(struct SocketBase *base asm("a6"));
VOID bsd_setservent(LONG stayOpen asm("d0"), struct SocketBase *base asm("a6"));
VOID bsd_endservent(struct SocketBase *base asm("a6"));
APTR bsd_getservent(struct SocketBase *base asm("a6"));
APTR bsd_gethostbyname_r(STRPTR name asm("a0"), APTR hp asm("a1"), APTR buf asm("a2"), ULONG buflen asm("d0"), LONG *he asm("a3"), struct SocketBase *base asm("a6"));
APTR bsd_gethostbyaddr_r(STRPTR addr asm("a0"), LONG len asm("d0"), LONG type asm("d1"), APTR hp asm("a1"), APTR buf asm("a2"), ULONG buflen asm("d2"), LONG *he asm("a3"), struct SocketBase *base asm("a6"));
LONG bsd_In_LocalAddr(ULONG address asm("d0"), struct SocketBase *base asm("a6"));
LONG bsd_In_CanForward(ULONG address asm("d0"), struct SocketBase *base asm("a6"));
LONG bsd_AddDomainNameServer(STRPTR address asm("a0"), struct SocketBase *base asm("a6"));
LONG bsd_RemoveDomainNameServer(STRPTR address asm("a0"), struct SocketBase *base asm("a6"));
APTR bsd_ObtainDomainNameServerList(struct SocketBase *base asm("a6"));
VOID bsd_ReleaseDomainNameServerList(APTR list asm("a0"), struct SocketBase *base asm("a6"));
LONG bsd_GetDefaultDomainName(STRPTR buffer asm("a0"), LONG bufferSize asm("d0"), struct SocketBase *base asm("a6"));
VOID bsd_SetDefaultDomainName(STRPTR buffer asm("a0"), struct SocketBase *base asm("a6"));

/* shared helpers (sb_misc.c) */
struct sb_hostent *sb_host_resolve(struct SocketBase *base, const char *name, ULONG *addrOut, LONG *herrOut);

#endif /* SB_BASE_H */
