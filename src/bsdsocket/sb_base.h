/* SPDX-License-Identifier: BSD-3-Clause */
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
#include <strutil.h>

#include "sb_config.h"

struct tcp_pcb;
struct udp_pcb;
struct raw_pcb;

/* case-insensitive ASCII string equality — the one comparer for every
 * netdb/config table (protocols, services, networks) */
static inline BOOL sb_strieq(const char *a, const char *b)
{
    return _Stricmp((CONST_STRPTR)a, (CONST_STRPTR)b) == 0;
}

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

struct sb_netent
{
    char *n_name;
    char **n_aliases;
    LONG n_addrtype;
    ULONG n_net; /* classful network number, host order */
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
#define SB_SO_SNDTIMEO 0x1005
#define SB_SO_RCVTIMEO 0x1006
#define SB_SO_ERROR 0x1007
#define SB_SO_TYPE 0x1008
#define SB_TCP_NODELAY 1

#define SB_MSG_OOB 0x01
#define SB_MSG_PEEK 0x02
#define SB_MSG_TRUNC 0x10  /* recvmsg: datagram was longer than the iov space */
#define SB_MSG_CTRUNC 0x20 /* recvmsg: ancillary data was truncated (never set) */
#define SB_MSG_WAITALL 0x40
#define SB_MSG_DONTWAIT 0x80

/* GetSocketEvents / SO_EVENTMASK (libraries/bsdsocket.h) */
#define SB_SO_EVENTMASK 0x2001
#define SB_FD_ACCEPT 0x01
#define SB_FD_CONNECT 0x02
#define SB_FD_OOB 0x04
#define SB_FD_READ 0x08
#define SB_FD_WRITE 0x10
#define SB_FD_ERROR 0x20
#define SB_FD_CLOSE 0x40

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

/* getaddrinfo/getnameinfo (netinclude/netdb.h) */
#define SB_PF_UNSPEC 0

#define SB_AI_PASSIVE 1
#define SB_AI_CANONNAME 2
#define SB_AI_NUMERICHOST 4
#define SB_AI_EXT 8
#define SB_AI_NUMERICSERV 16
#define SB_AI_MASK \
    (SB_AI_PASSIVE | SB_AI_CANONNAME | SB_AI_NUMERICHOST | SB_AI_NUMERICSERV)

#define SB_NI_NUMERICHOST 1
#define SB_NI_NUMERICSERV 2
#define SB_NI_NOFQDN 4
#define SB_NI_NAMEREQD 8
#define SB_NI_DGRAM 16
#define SB_NI_WITHSCOPEID 32

#define SB_EAI_BADFLAGS (-1)
#define SB_EAI_NONAME (-2)
#define SB_EAI_AGAIN (-3)
#define SB_EAI_FAIL (-4)
#define SB_EAI_NODATA (-5)
#define SB_EAI_FAMILY (-6)
#define SB_EAI_SOCKTYPE (-7)
#define SB_EAI_SERVICE (-8)
#define SB_EAI_MEMORY (-10)
#define SB_EAI_BADHINTS (-12)
#define SB_EAI_PROTOCOL (-13)

/* wire layout of netinclude/netdb.h struct addrinfo (all fields 32-bit) */
struct sb_addrinfo
{
    LONG ai_flags;
    LONG ai_family;
    LONG ai_socktype;
    LONG ai_protocol;
    ULONG ai_addrlen;
    struct sb_sockaddr_in *ai_addr;
    char *ai_canonname;
    struct sb_addrinfo *ai_next;
};

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
/* syslog configuration (openlog/setlogmask). The C runtimes wire these at
 * startup — clib2 sets SBTC_LOGTAGPTR to the program name during its socket
 * init, so failing them here aborts every clib2-linked networked program with
 * "bsdsocket.library could not be initialized". Consumed by bsd_vsyslog. */
#define SBTC_LOGSTAT 10
#define SBTC_LOGTAGPTR 11
#define SBTC_LOGFACILITY 12
#define SBTC_LOGMASK 13
/* <sys/syslog.h>: a message of priority p is logged when its bit is set in the
 * mask; the default mask enables all eight priorities. */
#define SB_LOG_PRIMASK 0x07
#define SB_LOG_MASK(pri) (1UL << ((pri) & SB_LOG_PRIMASK))
#define SB_LOGMASK_ALL 0xFFUL
#define SBTC_ERRNOBYTEPTR 21
#define SBTC_ERRNOWORDPTR 22
#define SBTC_ERRNOLONGPTR 24
#define SBTC_HERRNOLONGPTR 25
#define SBTC_RELEASESTRPTR 29 /* GET-only: stack-identifying version string */

/* Roadshow feature-capability tags (SBTM_GETREF(...) probes): an opener reads
 * these through SocketBaseTagList to learn which extension LVO groups this
 * library actually provides. */
#define SBTC_NUM_PACKET_FILTER_CHANNELS 40
#define SBTC_HAVE_ROUTING_API 41
#define SBTC_HAVE_INTERFACE_API 47
#define SBTC_HAVE_MONITORING_API 50
#define SBTC_HAVE_STATUS_API 53
#define SBTC_HAVE_DNS_API 54
#define SBTC_HAVE_LOCAL_DATABASE_API 59
#define SBTC_HAVE_ADDRESS_CONVERSION_API 60
#define SBTC_HAVE_KERNEL_MEMORY_API 61
#define SBTC_HAVE_SERVER_API 63
#define SBTC_HAVE_ROADSHOWDATA_API 67
#define SBTC_HAVE_GETHOSTADDR_R_API 69

/* status query tags (GET-only). SYSTEM_STATUS returns a bitmask of SBSYSSTAT_*;
 * GET_BYTES_* return an SBQUAD_T by reference (a 64-bit value cannot go by
 * value, so a VAL request is refused). */
#define SBTC_SYSTEM_STATUS 56
#define SBTC_GET_BYTES_RECEIVED 64
#define SBTC_GET_BYTES_SENT 65

/* SBTC_SYSTEM_STATUS result bits (loopback is not counted as an interface) */
#define SBSYSSTAT_Interfaces (1UL << 0)
#define SBSYSSTAT_PTP_Interfaces (1UL << 1)
#define SBSYSSTAT_BCast_Interfaces (1UL << 2)
#define SBSYSSTAT_Resolver (1UL << 3)
#define SBSYSSTAT_Routes (1UL << 4)
#define SBSYSSTAT_DefaultRoute (1UL << 5)

/* QueryInterfaceTagList tags (netinclude/libraries/bsdsocket.h). Only the
 * read-only address/config/link subset this stack answers is copied here;
 * IFQ_BASE = TAG_USER + 1900. Each tag's ti_Data points to caller storage
 * to fill (per the autodoc): a STRPTR/LONG/ULONG pointer for scalars, a raw
 * byte buffer for the hardware address, and a struct sb_sockaddr_in pointer
 * for the address tags. */
#define IFQ_BASE (TAG_USER + 1900)
#define IFQ_DeviceName (IFQ_BASE + 1)          /* STRPTR *  */
#define IFQ_DeviceUnit (IFQ_BASE + 2)          /* LONG *    */
#define IFQ_HardwareAddressSize (IFQ_BASE + 3) /* LONG * (bits) */
#define IFQ_HardwareAddress (IFQ_BASE + 4)     /* UBYTE * (<=16) */
#define IFQ_MTU (IFQ_BASE + 5)                 /* LONG *    */
#define IFQ_HardwareType (IFQ_BASE + 7)        /* LONG *    */
#define IFQ_Address (IFQ_BASE + 14)            /* struct sockaddr * */
#define IFQ_DestinationAddress (IFQ_BASE + 15) /* struct sockaddr * */
#define IFQ_BroadcastAddress (IFQ_BASE + 16)   /* struct sockaddr * */
#define IFQ_NetMask (IFQ_BASE + 17)            /* struct sockaddr_in * */
#define IFQ_State (IFQ_BASE + 19)              /* LONG * (SM_*) */
#define IFQ_AddressBindType (IFQ_BASE + 20)    /* LONG * (IFABT_*) */
#define IFQ_PrimaryDNSAddress (IFQ_BASE + 22)  /* struct sockaddr_in * */
#define IFQ_SecondaryDNSAddress (IFQ_BASE + 23) /* struct sockaddr_in * */
/* counter/status tags backed by the NIC-stats cache and lwIP stats */
#define IFQ_BPS (IFQ_BASE + 6)               /* LONG * (bits/second) */
#define IFQ_PacketsReceived (IFQ_BASE + 8)   /* ULONG *   */
#define IFQ_PacketsSent (IFQ_BASE + 9)       /* ULONG *   */
#define IFQ_BadData (IFQ_BASE + 10)          /* ULONG *   */
#define IFQ_Overruns (IFQ_BASE + 11)         /* ULONG *   */
#define IFQ_UnknownTypes (IFQ_BASE + 12)     /* ULONG *   */
#define IFQ_LastStart (IFQ_BASE + 13)        /* struct timeval * */
#define IFQ_Metric (IFQ_BASE + 18)           /* LONG *    */
#define IFQ_NumReadRequests (IFQ_BASE + 24)  /* LONG *    */
#define IFQ_NumWriteRequests (IFQ_BASE + 26) /* LONG *    */
#define IFQ_GetBytesIn (IFQ_BASE + 28)       /* SBQUAD_T * */
#define IFQ_GetBytesOut (IFQ_BASE + 29)      /* SBQUAD_T * */
#define IFQ_GetDebugMode (IFQ_BASE + 30)     /* LONG * (bool) */
#define IFQ_HardwareMTU (IFQ_BASE + 34)      /* LONG *    */
#define IFQ_OutputDrops (IFQ_BASE + 35)      /* LONG *    */
#define IFQ_InputDrops (IFQ_BASE + 36)       /* LONG *    */
#define IFQ_OutputErrors (IFQ_BASE + 37)     /* LONG *    */
#define IFQ_InputErrors (IFQ_BASE + 38)      /* LONG *    */
#define IFQ_IPDrops (IFQ_BASE + 41)          /* LONG *    */
#define IFQ_ARPDrops (IFQ_BASE + 42)         /* LONG *    */

/* SBQUAD_T (libraries/bsdsocket.h): a 64-bit counter, high word first. Used
 * by the IFQ_GetBytes* tags and the SBTC_GET_BYTES_* SocketBaseTagList tags. */
struct sb_squad
{
    ULONG sbq_High;
    ULONG sbq_Low;
};

/* the one NetDevU64 -> SBQUAD_T mapping, shared by both byte-counter surfaces */
static inline void sb_squad_from_u64(struct sb_squad *q, struct NetDevU64 v)
{
    q->sbq_High = v.ndu_Hi;
    q->sbq_Low = v.ndu_Lo;
}

/* IFQ_AddressBindType values */
#define IFABT_Unknown 0
#define IFABT_Static 1
#define IFABT_Dynamic 2

/* IFQ_State values (subset: this stack reports only down/up) */
#define SM_Offline 0
#define SM_Online 1
#define SM_Down 2
#define SM_Up 3

/* SANA-II hardware type reported for Ethernet interfaces (S2WireType_Ethernet) */
#define SB_S2WIRETYPE_ETHERNET 1

struct sb_linger
{
    LONG l_onoff;
    LONG l_linger;
};

/* --- sockets -------------------------------------------------------------- */

#define SB_FD_COUNT 64  /* per-opener default (AmiTCP minimum) */
#define SB_FD_MAX 256   /* SBTC_DTABLESIZE growth ceiling; sizes WaitSelect's
                         * stack-local fd_set copies, so keep it modest */
#define SB_DGRAM_QMAX 32
#define SB_ACCEPT_QMAX 16
#define SB_HOSTNAME_MAX 128
#define SB_SOCK_OWNERS 4 /* distinct owner bases a shared socket can wake */

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

/* One owner base of a socket: the task woken on readiness. A plain socket has
 * exactly one (its creator). A socket shared by ReleaseCopyOfSocket +
 * ObtainSocket gains a second — both holders must be woken. fdrefs counts the
 * fds THIS base holds so a partial close (Dup2Socket left another fd open)
 * keeps the base an owner. An empty slot has base == NULL. */
struct SbOwnerRef
{
    struct SocketBase *base;
    UWORD fdrefs;
    UWORD pad;
};

struct SbSocket
{
    struct MinNode node; /* accept-queue linkage */
    /* owner bases whose tasks get woken; slot 0 is the sole owner in the
     * common case, extra slots fill only via ReleaseCopyOfSocket sharing.
     * All-empty == parked by ReleaseSocket (nobody to wake until obtained). */
    struct SbOwnerRef owners[SB_SOCK_OWNERS];
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

    UBYTE lingerOn;  /* SO_LINGER; on + time 0 => abort (RST) on close */
    UBYTE forceRst;  /* linger deadline expired: teardown must abort (RST) */
    UWORD lingerTime; /* seconds */

    union
    {
        struct tcp_pcb *tcp;
        struct udp_pcb *udp;
        struct raw_pcb *raw;
        APTR any;
    } pcb;

    LONG err; /* pending errno; pcb.any == NULL after fatal errors */

    ULONG sndTimeoMs; /* SO_SNDTIMEO/SO_RCVTIMEO, 0 = block forever */
    ULONG rcvTimeoMs;

    ULONG eventMask;     /* SO_EVENTMASK: FD_* events the app wants */
    ULONG eventsPending; /* accumulated, drained by GetSocketEvents */

    /* TCP receive backlog: tail-linked pbuf queue. Only per-pbuf len
     * fields are trusted for accounting — a pbuf chain's tot_len is u16
     * and the backlog under a 256 KB window overflows it; rxBytes is the
     * truth. */
    struct pbuf *rxq;
    struct pbuf *rxqTail;
    ULONG rxBytes;

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
    struct Library libNode; /* Exec library node; must stay first */
    ULONG segList;          /* our seglist, handed back by LibExpunge to unload us */
    struct SocketBase *root; /* NULL in the root base itself */

    /* --- root-only state --- */
    struct SignalSemaphore openLock; /* guards stack startup/shutdown */
    struct Task *stackTask;         /* the lwIP stack task; NULL while the stack is down */
    APTR sockPool;      /* objects; alloc/free under the core lock only */
    ULONG openCount;    /* child bases alive */
    struct MinList releasedSockets; /* ReleaseSocket parking lot (core lock) */
    LONG nextSockId;    /* ReleaseSocket UNIQUE_ID allocator; wraps to 1 rather
                         * than going negative, and skips ids still parked */
    char defaultDomain[SB_HOSTNAME_MAX]; /* Set/GetDefaultDomainName; "" = unset */
    /* startup configuration: written once by the stack task before
     * sb_stack_start() returns (i.e. before the first OpenLibrary()
     * completes), read-only afterwards — no locking needed */
    struct SbNetConfig netCfg;

    /* NIC statistics cache. Refreshed once/second by the stack task, which
     * owns devIO and holds NO core lock while polling. NEVER issue a netdev
     * DoIO (GET_STATS/GET_LINK) under netstack_lock: the driver's unit task
     * also takes that lock in its RX path, so a DoIO-under-lock deadlocks.
     * Readers take the core lock. netStatsValid is FALSE until the first good
     * poll and whenever no NIC is attached (counters then read zero). */
    struct NetDevStats netStats;
    struct NetDevLinkState netLink;
    struct sb_timeval netLastStart; /* IFQ_LastStart; zero until NETDEV_CMD_START */
    ULONG dgramRxDrops;    /* datagrams dropped at the socket queue (udpstat fullsock) */
    UBYTE netStatsValid;
    UBYTE pad2[3];

    /* --- per-opener state (child bases; garbage in the root) --- */
    struct Task *task;  /* the opener; the Signal() target of every wake */
    BYTE sigBit;        /* the wait bit of the header's blocking pattern; -1 if AllocSignal failed */
    UBYTE pad0[3];
    ULONG breakMask;    /* SBTC_BREAKMASK; aborts a blocking call with SB_EINTR (default SIGBREAKF_CTRL_C) */
    ULONG sigIoMask;    /* SBTC_SIGIOMASK; async SIGIO, delivered on every readiness change (see sb_wake) */
    ULONG sigUrgMask;   /* SBTC_SIGURGMASK; stored and reported back, never delivered — no OOB support */
    ULONG sigEventMask; /* SBTC_SIGEVENTMASK; stored, delivered with SIGIO */

    LONG internalErrno; /* the errno cell itself, used until SetErrnoPtr redirects errnoPtr */
    APTR errnoPtr;   /* defaults to &internalErrno */
    ULONG errnoSize; /* 1, 2 or 4 */
    LONG hErrno;     /* the h_errno counterpart of internalErrno */
    LONG *hErrnoPtr; /* defaults to &hErrno */

    /* syslog configuration (SBTC_LOG*; see bsd_vsyslog). logMask defaults to
     * SB_LOGMASK_ALL so an opener that never calls setlogmask logs every
     * priority, matching BSD. */
    ULONG logStat;     /* openlog() options (LOG_PID, ...) — advisory */
    STRPTR logTagPtr;  /* ident string prefixed to each message */
    ULONG logFacility; /* default facility — advisory */
    ULONG logMask;     /* setlogmask() priority bitmask */

    struct SbSocket **fd; /* fdCount entries; grown via SBTC_DTABLESIZE */
    ULONG fdCount;        /* SB_FD_COUNT..SB_FD_MAX */

    struct MsgPort *timerPort;   /* WaitSelect and SO_SNDTIMEO/SO_RCVTIMEO deadlines */
    struct timerequest *timerReq; /* UNIT_MICROHZ; one request, reused per blocking call */
    UBYTE timerOpen;   /* timer.device opened; FALSE => sb_wait_to falls back to blocking forever */
    UBYTE dnsDone;     /* handshake cell: the resolver callback sets it, then signals sigBit */
    UBYTE pad1[2];

    /* DNS + netdb per-opener result storage. The netdb calls return pointers
     * straight into these fields, so each result stays valid only until the
     * same opener's next call in that family — the classic BSD contract, and
     * why they live per-base rather than in the root. */
    ip_addr_t dnsAddr; /* dns_gethostbyname result cell, filled by sb_dns_cb */
    LONG dnsErr;       /* 0 or SB_HOST_NOT_FOUND, from the same callback */
    struct sb_hostent host;         /* gethostbyname/byaddr result; points at the fields below */
    char hostName[SB_HOSTNAME_MAX]; /* h_name backing store */
    ULONG hostAddr;        /* network order; the single address h_addr_list[0] points at */
    char *hostAddrList[2]; /* { &hostAddr, NULL } — always one address */
    char *hostAliases[1];  /* always { NULL } — the resolver never reports aliases */
    struct sb_protoent proto; /* getprotobyname/bynumber/getprotoent result */
    char *protoAliases[1];    /* always { NULL } */
    struct sb_servent serv;   /* getservbyname/byport/getservent result */
    char *servAliases[3];     /* up to 2 aliases + NULL terminator */
    struct sb_netent net;     /* getnetbyname/byaddr/getnetent result */
    char *netAliases[1];      /* always { NULL } */
    ULONG protoIdx; /* get*ent iterators */
    ULONG servIdx;
    ULONG netIdx;
    char ntoaBuf[20]; /* Inet_NtoA return buffer */
};

#define SB_ROOT(b) ((b)->root != NULL ? (b)->root : (b))

/* --- internals ------------------------------------------------------------- */

/* errno plumbing (sb_errno.c); sb_fail = set errno, return -1 */
void sb_set_errno(struct SocketBase *base, LONG code);
void sb_set_herrno(struct SocketBase *base, LONG code);
LONG sb_fail(struct SocketBase *base, LONG code);

/* socket core (sb_socket.c) */
struct SbSocket *sb_sock_alloc(struct SocketBase *base, SbSockType type);
void sb_sock_free(struct SocketBase *base, struct SbSocket *s); /* under lock */
LONG sb_fd_alloc(struct SocketBase *base, struct SbSocket *s);
struct SbSocket *sb_fd_get(struct SocketBase *base, LONG fd);

/* socket ownership (sb_socket.c); all under the core lock. incref adds one fd
 * reference for @b (a new owner slot on first, else fdrefs++), returning FALSE
 * only if the socket already has SB_SOCK_OWNERS distinct owners. decref drops
 * one, freeing the slot at zero. first == NULL only for a parked socket. */
BOOL sb_owner_incref(struct SbSocket *s, struct SocketBase *b);
void sb_owner_decref(struct SbSocket *s, struct SocketBase *b);
struct SocketBase *sb_owner_first(struct SbSocket *s);
void sb_wake(struct SbSocket *s);
void sb_event(struct SbSocket *s, ULONG ev); /* under lock; signals sigEventMask */
LONG sb_wait(struct SocketBase *base); /* 0 or SB_EINTR; drops+retakes the lock */

/* sb_wait with a per-call deadline (SO_RCVTIMEO/SO_SNDTIMEO). Zero-init the
 * SbTimedWait per API call; the first blocking iteration latches the
 * deadline. Adds SB_EWOULDBLOCK to sb_wait's returns; reclaims the opener's
 * timer before returning, so no cleanup is needed on any exit path. */
struct SbTimedWait
{
    ULONG deadlineMs;
    UBYTE set;
};
LONG sb_wait_to(struct SocketBase *base, ULONG ms, struct SbTimedWait *tw);
void sb_tcp_wire(struct SbSocket *s);  /* attach lwIP callbacks to s->pcb.tcp */
err_t sb_tcp_connected_cb(void *arg, struct tcp_pcb *tpcb, err_t err);
void sb_listen_wire(struct SbSocket *s);
void sb_udp_wire(struct SbSocket *s);
void sb_raw_wire(struct SbSocket *s);
BOOL sb_sock_readable(const struct SbSocket *s);
BOOL sb_sock_writable(struct SbSocket *s);
LONG sb_map_err(signed char lwip_err);
/* TCP peer address readout (sb_socket.c); core lock held, 0/0 without a pcb */
void sb_peer_ip(struct SbSocket *s, ULONG *addr, UWORD *port);

/* app sockaddr_in conversion (sb_api.c); shared with the sb_io.c data path */
LONG sb_addr_in(const struct sb_sockaddr_in *sa, LONG salen, ip_addr_t *ip, u16_t *port);
void sb_addr_out(APTR name, LONG *namelen, ULONG addr, UWORD port);

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

/* sb_api.c — socket lifecycle and control */
LONG bsd_socket(LONG domain asm("d0"), LONG type asm("d1"), LONG protocol asm("d2"), struct SocketBase *base asm("a6"));
LONG bsd_bind(LONG sock asm("d0"), APTR name asm("a0"), LONG namelen asm("d1"), struct SocketBase *base asm("a6"));
LONG bsd_listen(LONG sock asm("d0"), LONG backlog asm("d1"), struct SocketBase *base asm("a6"));
LONG bsd_accept(LONG sock asm("d0"), APTR addr asm("a0"), APTR addrlen asm("a1"), struct SocketBase *base asm("a6"));
LONG bsd_connect(LONG sock asm("d0"), APTR name asm("a0"), LONG namelen asm("d1"), struct SocketBase *base asm("a6"));
LONG bsd_shutdown(LONG sock asm("d0"), LONG how asm("d1"), struct SocketBase *base asm("a6"));
LONG bsd_CloseSocket(LONG sock asm("d0"), struct SocketBase *base asm("a6"));
LONG bsd_getsockname(LONG sock asm("d0"), APTR name asm("a0"), APTR namelen asm("a1"), struct SocketBase *base asm("a6"));
LONG bsd_getpeername(LONG sock asm("d0"), APTR name asm("a0"), APTR namelen asm("a1"), struct SocketBase *base asm("a6"));
LONG bsd_IoctlSocket(LONG sock asm("d0"), ULONG req asm("d1"), APTR argp asm("a0"), struct SocketBase *base asm("a6"));

/* sb_io.c — the data path */
LONG bsd_sendto(LONG sock asm("d0"), APTR buf asm("a0"), LONG len asm("d1"), LONG flags asm("d2"), APTR to asm("a1"), LONG tolen asm("d3"), struct SocketBase *base asm("a6"));
LONG bsd_send(LONG sock asm("d0"), APTR buf asm("a0"), LONG len asm("d1"), LONG flags asm("d2"), struct SocketBase *base asm("a6"));
LONG bsd_recvfrom(LONG sock asm("d0"), APTR buf asm("a0"), LONG len asm("d1"), LONG flags asm("d2"), APTR addr asm("a1"), APTR addrlen asm("a2"), struct SocketBase *base asm("a6"));
LONG bsd_recv(LONG sock asm("d0"), APTR buf asm("a0"), LONG len asm("d1"), LONG flags asm("d2"), struct SocketBase *base asm("a6"));
LONG bsd_sendmsg(LONG sock asm("d0"), APTR msg asm("a0"), LONG flags asm("d1"), struct SocketBase *base asm("a6"));
LONG bsd_recvmsg(LONG sock asm("d0"), APTR msg asm("a0"), LONG flags asm("d1"), struct SocketBase *base asm("a6"));

/* sb_sockopt.c — options, events, signals */
LONG bsd_setsockopt(LONG sock asm("d0"), LONG level asm("d1"), LONG optname asm("d2"), APTR optval asm("a0"), LONG optlen asm("d3"), struct SocketBase *base asm("a6"));
LONG bsd_getsockopt(LONG sock asm("d0"), LONG level asm("d1"), LONG optname asm("d2"), APTR optval asm("a0"), APTR optlen asm("a1"), struct SocketBase *base asm("a6"));
LONG bsd_GetSocketEvents(ULONG *eventsp asm("a0"), struct SocketBase *base asm("a6"));
LONG bsd_getdtablesize(struct SocketBase *base asm("a6"));
VOID bsd_SetSocketSignals(ULONG intMask asm("d0"), ULONG ioMask asm("d1"), ULONG urgMask asm("d2"), struct SocketBase *base asm("a6"));

/* sb_select.c */
LONG bsd_WaitSelect(LONG nfds asm("d0"), APTR readfds asm("a0"), APTR writefds asm("a1"), APTR exceptfds asm("a2"), APTR timeout asm("a3"), ULONG *signals asm("d1"), struct SocketBase *base asm("a6"));

/* sb_errno.c */
LONG bsd_Errno(struct SocketBase *base asm("a6"));
VOID bsd_SetErrnoPtr(APTR errnoPtr asm("a0"), LONG size asm("d0"), struct SocketBase *base asm("a6"));

/* sb_taglist.c */
LONG bsd_SocketBaseTagList(struct TagItem *tags asm("a0"), struct SocketBase *base asm("a6"));

/* sb_inet.c — address conversion and classification */
STRPTR bsd_Inet_NtoA(ULONG ip asm("d0"), struct SocketBase *base asm("a6"));
ULONG bsd_inet_addr(STRPTR cp asm("a0"), struct SocketBase *base asm("a6"));
LONG bsd_inet_aton(STRPTR cp asm("a0"), APTR addr asm("a1"), struct SocketBase *base asm("a6"));
STRPTR bsd_inet_ntop(LONG af asm("d0"), APTR src asm("a0"), STRPTR dst asm("a1"), LONG size asm("d1"), struct SocketBase *base asm("a6"));
LONG bsd_inet_pton(LONG af asm("d0"), STRPTR src asm("a0"), APTR dst asm("a1"), struct SocketBase *base asm("a6"));
ULONG bsd_Inet_LnaOf(ULONG in asm("d0"), struct SocketBase *base asm("a6"));
ULONG bsd_Inet_NetOf(ULONG in asm("d0"), struct SocketBase *base asm("a6"));
ULONG bsd_Inet_MakeAddr(ULONG net asm("d0"), ULONG host asm("d1"), struct SocketBase *base asm("a6"));
ULONG bsd_inet_network(STRPTR cp asm("a0"), struct SocketBase *base asm("a6"));
LONG bsd_In_LocalAddr(ULONG address asm("d0"), struct SocketBase *base asm("a6"));
LONG bsd_In_CanForward(ULONG address asm("d0"), struct SocketBase *base asm("a6"));

/* sb_resolver.c — DNS + hostent */
APTR bsd_gethostbyname(STRPTR name asm("a0"), struct SocketBase *base asm("a6"));
APTR bsd_gethostbyaddr(STRPTR addr asm("a0"), LONG len asm("d0"), LONG type asm("d1"), struct SocketBase *base asm("a6"));
APTR bsd_gethostbyname_r(STRPTR name asm("a0"), APTR hp asm("a1"), APTR buf asm("a2"), ULONG buflen asm("d0"), LONG *he asm("a3"), struct SocketBase *base asm("a6"));
APTR bsd_gethostbyaddr_r(STRPTR addr asm("a0"), LONG len asm("d0"), LONG type asm("d1"), APTR hp asm("a1"), APTR buf asm("a2"), ULONG buflen asm("d2"), LONG *he asm("a3"), struct SocketBase *base asm("a6"));
LONG bsd_gethostname(STRPTR name asm("a0"), LONG namelen asm("d0"), struct SocketBase *base asm("a6"));
ULONG bsd_gethostid(struct SocketBase *base asm("a6"));

/* sb_netdb.c — protocols/services/networks databases */
APTR bsd_getprotobyname(STRPTR name asm("a0"), struct SocketBase *base asm("a6"));
APTR bsd_getprotobynumber(LONG proto asm("d0"), struct SocketBase *base asm("a6"));
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

/* sb_syslog.c */
VOID bsd_vsyslog(LONG pri asm("d0"), STRPTR msg asm("a0"), APTR args asm("a1"), struct SocketBase *base asm("a6"));

/* sb_sockpass.c — fd duplication and task handoff */
LONG bsd_Dup2Socket(LONG oldSock asm("d0"), LONG newSock asm("d1"), struct SocketBase *base asm("a6"));
LONG bsd_ObtainSocket(LONG id asm("d0"), LONG domain asm("d1"), LONG type asm("d2"), LONG protocol asm("d3"), struct SocketBase *base asm("a6"));
LONG bsd_ReleaseSocket(LONG sock asm("d0"), LONG id asm("d1"), struct SocketBase *base asm("a6"));
LONG bsd_ReleaseCopyOfSocket(LONG sock asm("d0"), LONG id asm("d1"), struct SocketBase *base asm("a6"));

/* sb_dnsconfig.c — resolver configuration */
LONG bsd_AddDomainNameServer(STRPTR address asm("a0"), struct SocketBase *base asm("a6"));
LONG bsd_RemoveDomainNameServer(STRPTR address asm("a0"), struct SocketBase *base asm("a6"));
APTR bsd_ObtainDomainNameServerList(struct SocketBase *base asm("a6"));
VOID bsd_ReleaseDomainNameServerList(APTR list asm("a0"), struct SocketBase *base asm("a6"));
LONG bsd_GetDefaultDomainName(STRPTR buffer asm("a0"), LONG bufferSize asm("d0"), struct SocketBase *base asm("a6"));
VOID bsd_SetDefaultDomainName(STRPTR buffer asm("a0"), struct SocketBase *base asm("a6"));

/* sb_ifquery.c — read-only interface status (the config family is declined) */
APTR bsd_ObtainInterfaceList(struct SocketBase *base asm("a6"));
VOID bsd_ReleaseInterfaceList(APTR list asm("a0"), struct SocketBase *base asm("a6"));
LONG bsd_QueryInterfaceTagList(STRPTR name asm("a0"), struct TagItem *tags asm("a1"), struct SocketBase *base asm("a6"));
/* shared graceful-refuse stub for the interface-config LVOs (errno = EINVAL) */
LONG bsd_InterfaceConfigUnsupported(struct SocketBase *base asm("a6"));

/* sb_netstat.c */
LONG bsd_GetNetworkStatistics(LONG type asm("d0"), LONG version asm("d1"), APTR destination asm("a0"), LONG size asm("d2"), struct SocketBase *base asm("a6"));

/* sb_gai.c */
LONG bsd_getaddrinfo(STRPTR hostname asm("a0"), STRPTR servname asm("a1"), struct sb_addrinfo *hints asm("a2"), struct sb_addrinfo **res asm("a3"), struct SocketBase *base asm("a6"));
VOID bsd_freeaddrinfo(struct sb_addrinfo *ai asm("a0"), struct SocketBase *base asm("a6"));
STRPTR bsd_gai_strerror(LONG errnum asm("a0"), struct SocketBase *base asm("a6"));
LONG bsd_getnameinfo(APTR sa asm("a0"), ULONG salen asm("d0"), STRPTR host asm("a1"), ULONG hostlen asm("d1"), STRPTR serv asm("a2"), ULONG servlen asm("d2"), ULONG flags asm("d3"), struct SocketBase *base asm("a6"));

/* the blocking resolver core (sb_resolver.c); shared with sb_gai.c */
struct sb_hostent *sb_host_resolve(struct SocketBase *base, const char *name, ULONG *addrOut, LONG *herrOut);

/* reverse DNS (sb_rdns.c): resolve @addr (network order) to a hostname via a
 * PTR query to the configured resolver. Fills @out (NUL-terminated) and
 * returns 0 on success, -1 if no PTR record, no resolver, or timeout. Takes
 * the core lock internally; call it without the lock held. */
LONG sb_ptr_resolve(struct SocketBase *base, ULONG addr, char *out, ULONG outmax);

/* services table lookups (sb_netdb.c); ports host-order, proto "tcp"/"udp" */
LONG sb_serv_port_by_name(const char *name, const char *proto);
const char *sb_serv_name_by_port(UWORD port, const char *proto);

#endif /* SB_BASE_H */
