/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * bsdsocket.library netstack control port — the private protocol the
 * AddNetInterface / RemoveNetInterface / NetShutdown commands use to add
 * and remove network interfaces and to stop the stack.
 *
 * The stack task owns a public MsgPort named NETCTL_PORT_NAME for its
 * whole lifetime; a client fills a struct NetCtlMsg, PutMsg()s it and
 * waits for the reply. Interface configuration is parsed from
 * DEVS:NetInterfaces/<name> files by the AddNetInterface command; the
 * stack only ever sees the compact struct NetCtlIfConfig.
 *
 * Reply timing: every op is answered from the stack task. ADD_IF is
 * answered only once the interface is operational — link up for a static
 * config, DHCP lease bound for a dynamic one — so a client that wants a
 * timeout keeps its own timer and sends CANCEL_ADD. SHUTDOWN is answered
 * only when the teardown finished (or CANCEL_SHUTDOWN recalled it); the
 * OK reply to SHUTDOWN is the very last act of the stack task, after
 * which the library is expunge-able.
 *
 * Private to this component (library + tools built together): no
 * compatibility promise beyond ncm_Version, which the library rejects
 * when it does not match.
 */

#ifndef NETSTACK_CTL_H
#define NETSTACK_CTL_H

#include <exec/ports.h>
#include <exec/types.h>

#define NETCTL_PORT_NAME "bsdsocket.netctl"
#define NETCTL_VERSION   2

#define NETCTL_IFNAME_MAX 16 /* Roadshow-compatible: 15 chars + NUL */
#define NETCTL_DEV_MAX    64 /* OpenDevice name, path form included */
#define NETCTL_ID_MAX     64 /* a hostname; sizes nif_Id and, via sb_config.h,
                                the global HOSTNAME pref it defaults to */

/* ncm_Op */
#define NETCTL_OP_ADD_IF          0 /* attach + configure ncm_Config; the reply
                                       waits for the interface to become
                                       operational (see above) */
#define NETCTL_OP_CANCEL_ADD      1 /* recall a pending ADD_IF reply; the
                                       interface stays added and keeps trying */
#define NETCTL_OP_REM_IF          2 /* remove by ncm_Config.nif_Name; ncm_Force
                                       overrides the in-use refusal */
#define NETCTL_OP_SHUTDOWN        3 /* stop every interface and the stack task;
                                       clients are asked to close first */
#define NETCTL_OP_CANCEL_SHUTDOWN 4 /* recall a pending SHUTDOWN */
/* 5+ reserved (interface state/config ops) */

/* ncm_Result */
#define NETCTL_OK               0
#define NETCTL_ERR_VERSION      (-1)  /* ncm_Version != NETCTL_VERSION */
#define NETCTL_ERR_OP           (-2)  /* unknown ncm_Op */
#define NETCTL_ERR_PARAM        (-3)  /* bad name or config field */
#define NETCTL_ERR_EXISTS       (-4)  /* ADD_IF: an interface is already attached
                                         (or being added); SHUTDOWN: one pending */
#define NETCTL_ERR_NOTFOUND     (-5)  /* REM_IF: no such interface;
                                         CANCEL_*: nothing pending */
#define NETCTL_ERR_BUSY         (-6)  /* REM_IF without force: sockets bound to
                                         the interface; count in ncm_Count */
#define NETCTL_ERR_DEVICE       (-7)  /* OpenDevice/ATTACH/START failed;
                                         device io_Error in ncm_Aux */
#define NETCTL_ERR_NOMEM        (-8)
#define NETCTL_ERR_INACTIVE     (-9)  /* stack shutting down / port withdrawn */
#define NETCTL_ERR_PENDING      (-10) /* ADD_IF reply after CANCEL_ADD: the
                                         interface was added and configured but
                                         is not operational yet (no link, or no
                                         DHCP lease) — it keeps trying in the
                                         background */
#define NETCTL_ERR_ABORTED      (-11) /* SHUTDOWN reply after CANCEL_SHUTDOWN;
                                         remaining client count in ncm_Count */
#define NETCTL_ERR_HWTYPE       (-12) /* ADD_IF: the SANA-II device is not
                                         48-bit Ethernet (wrong wire type) */

/* nif_Flags */
#define NETCTL_IFF_DHCP     (1UL << 0) /* configure via DHCP (no static address) */
#define NETCTL_IFF_HAS_MASK (1UL << 1)
#define NETCTL_IFF_HAS_GW   (1UL << 2)
#define NETCTL_IFF_HAS_MTU  (1UL << 3)

/* nif_Type — which driver ABI the device speaks. AUTO (the default, and 0 so
 * an old-style config parses to it) probes with NSCMD_DEVICEQUERY:
 * NSDEVTYPE_SANA2 -> SANA2, NETDEV_CMD_ATTACH in the command list -> NETDEV,
 * and a device without NSD support is assumed SANA-II (legacy drivers
 * predate NSD; every netdev driver implements it). */
#define NETCTL_TYPE_AUTO   0
#define NETCTL_TYPE_NETDEV 1
#define NETCTL_TYPE_SANA2  2

/* Parsed by the command (from a DEVS:NetInterfaces/<name> file), executed by
 * the stack task. IPv4 addresses are raw network-byte-order words so the
 * tools never need lwIP headers. */
struct NetCtlIfConfig
{
    char  nif_Name[NETCTL_IFNAME_MAX]; /* = FilePart(config file), <= 15 chars */
    char  nif_Device[NETCTL_DEV_MAX];  /* as written; the stack retries the
                                          bare basename for resident modules */
    LONG  nif_Unit;
    LONG  nif_Type;                    /* NETCTL_TYPE_* driver-ABI selection */
    ULONG nif_Flags;                   /* NETCTL_IFF_* */
    ULONG nif_Addr;                    /* network byte order; 0 = unset */
    ULONG nif_Mask;
    ULONG nif_Gateway;
    LONG  nif_Mtu;                     /* -> nda_MtuReq; 0 = driver default */
    LONG  nif_VlanTci;                 /* -1 = untagged; else (pcp<<13)|vid */
    char  nif_Id[NETCTL_ID_MAX];       /* DHCP client hostname (option 12);
                                          "" = the global HOSTNAME pref */
};

struct NetCtlMsg
{
    struct Message ncm_Msg;
    UWORD ncm_Version; /* NETCTL_VERSION */
    UWORD ncm_Op;
    LONG  ncm_Result;
    LONG  ncm_Aux;     /* NETCTL_ERR_DEVICE: the device's io_Error */
    ULONG ncm_Force;   /* REM_IF: nonzero = remove even when in use */
    ULONG ncm_Count;   /* ERR_BUSY: bound sockets; ERR_ABORTED: open clients */

    struct NetCtlIfConfig ncm_Config; /* ADD_IF in full; REM_IF and CANCEL_ADD
                                         read nif_Name only */

    /* ADD_IF success outputs (network byte order) */
    ULONG ncm_AddrOut;
    ULONG ncm_MaskOut;
    ULONG ncm_GatewayOut;
    ULONG ncm_DnsOut[2];
};

#endif /* NETSTACK_CTL_H */
