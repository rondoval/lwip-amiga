/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The netstack control-port server: runtime interface add/remove (and, with
 * it, stack shutdown) over the private netstack_ctl.h protocol. This is what
 * the AddNetInterface / RemoveNetInterface / NetShutdown commands talk to.
 *
 * Everything runs on the stack task, which serializes all of it: one message
 * is handled at a time, and the netdev bring-up/teardown helpers
 * (sb_stack_priv.h) already run on this task by construction — OpenDevice
 * needs a Process, and a netdev DoIO must never happen under the core lock.
 *
 * Reply discipline: a handler either answers the message inline or parks it
 * (returns SB_NETCTL_PARKED). Parked messages are always answered
 * eventually — by the tick poll (DHCP bound), a CANCEL, a REMOVE of the
 * interface, or teardown. No message is ever dropped.
 */

#include "sb_base.h"

#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#include <clib/timer_protos.h>
#else
#include <proto/exec.h>
#define TIMER_BASE_NAME TimerBase
#include <proto/timer.h>
#endif

#include <debug.h>

#include <lwip/dhcp.h>
#include <lwip/dns.h>
#include <lwip/netif.h>
#include <lwip/priv/tcp_priv.h>
#include <lwip/tcp.h>
#include <lwip/udp.h>

#include "netstack.h"
#include "sb_netctl.h"
#include "sb_stack_priv.h"

/* internal handler verdict: the message was parked, do not reply (all public
 * NETCTL_* results are <= 0) */
#define SB_NETCTL_PARKED 1

static struct MsgPort *sbCtlPort;

LONG sb_netctl_start(void)
{
    sbCtlPort = CreateMsgPort();
    if (sbCtlPort == NULL)
        return -1;
    sbCtlPort->mp_Node.ln_Name = (char *)NETCTL_PORT_NAME;
    sbCtlPort->mp_Node.ln_Pri = 0;
    AddPort(sbCtlPort);
    KprintfT("[bsdsocket] netctl: port up\n");
    return 0;
}

void sb_netctl_stop(struct SbStackCtx *ctx)
{
    if (sbCtlPort == NULL)
        return;

    /* Unpublish first: a client's FindPort+PutMsg runs under its own
     * Forbid(), so after this it either never saw the port or its message
     * is already queued — and the drain below answers those. */
    Forbid();
    RemPort(sbCtlPort);
    Permit();

    if (ctx->pendingAdd != NULL)
    {
        ctx->pendingAdd->ncm_Result = NETCTL_ERR_INACTIVE;
        ReplyMsg(&ctx->pendingAdd->ncm_Msg);
        ctx->pendingAdd = NULL;
    }

    struct NetCtlMsg *msg;
    while ((msg = (struct NetCtlMsg *)GetMsg(sbCtlPort)) != NULL)
    {
        msg->ncm_Result = NETCTL_ERR_INACTIVE;
        ReplyMsg(&msg->ncm_Msg);
    }
    DeleteMsgPort(sbCtlPort);
    sbCtlPort = NULL;
}

ULONG sb_netctl_sigmask(void)
{
    return sbCtlPort != NULL ? (1UL << sbCtlPort->mp_SigBit) : 0;
}

/* the ADD success outputs: what the interface actually got (core lock) */
static void sb_netctl_fill_outputs(struct SbStackCtx *ctx, struct NetCtlMsg *msg)
{
    netstack_lock();
    struct netif *nf = &sb_ctx_base(ctx)->nib_Netif;
    msg->ncm_AddrOut = ip4_addr_get_u32(netif_ip4_addr(nf));
    msg->ncm_MaskOut = ip4_addr_get_u32(netif_ip4_netmask(nf));
    msg->ncm_GatewayOut = ip4_addr_get_u32(netif_ip4_gw(nf));
    msg->ncm_DnsOut[0] = ip4_addr_get_u32(ip_2_ip4(dns_getserver(0)));
    msg->ncm_DnsOut[1] = ip4_addr_get_u32(ip_2_ip4(dns_getserver(1)));
    netstack_unlock();
}

/* explicitly configured DNS servers beat whatever a DHCP lease installed */
static void sb_netctl_apply_dns(struct SocketBase *root)
{
    netstack_lock();
    for (u8_t i = 0; i < 2; i++)
    {
        if (!ip4_addr_isany_val(root->netCfg.cfg_Dns[i]))
            dns_setserver(i, &root->netCfg.cfg_Dns[i]);
    }
    netstack_unlock();
}

static void sb_netctl_stamp_start(struct SbStackCtx *ctx)
{
    struct timeval tv;
    GetSysTime(&tv);
    ctx->root->netLastStart.tv_secs = tv.tv_secs;
    ctx->root->netLastStart.tv_micro = tv.tv_micro;
}

/* Answer a parked ADD_IF once the interface is operational: link up for a
 * static config, DHCP lease bound for a dynamic one (a lease implies link).
 * TRUE when replied. */
static BOOL sb_netctl_try_complete_add(struct SbStackCtx *ctx)
{
    struct NetCtlMsg *msg = ctx->pendingAdd;
    if (msg == NULL)
        return FALSE;

    struct netif *nf = &sb_ctx_base(ctx)->nib_Netif;
    netstack_lock();
    BOOL ready = (msg->ncm_Config.nif_Flags & NETCTL_IFF_DHCP)
                     ? dhcp_supplied_address(nf) != 0
                     : netif_is_link_up(nf) != 0;
    netstack_unlock();
    if (!ready)
        return FALSE;

    sb_netctl_apply_dns(ctx->root);
    sb_netctl_fill_outputs(ctx, msg);

    /* the address line came from the netif observer; this adds what only
     * the completed add knows — the resolver configuration in effect */
    char dns1[IP4ADDR_STRLEN_MAX];
    char dns2[IP4ADDR_STRLEN_MAX];
    ip4_addr_t a;
    a.addr = msg->ncm_DnsOut[0];
    ip4addr_ntoa_r(&a, dns1, sizeof(dns1));
    a.addr = msg->ncm_DnsOut[1];
    ip4addr_ntoa_r(&a, dns2, sizeof(dns2));
    SB_LOG(NS_LOG_NOTICE, "%s: operational, DNS %s %s", sb_ctx_base(ctx)->nib_Name, dns1, dns2);

    msg->ncm_Result = NETCTL_OK;
    ReplyMsg(&msg->ncm_Msg);
    ctx->pendingAdd = NULL;
    return TRUE;
}

static LONG sb_netctl_add(struct SbStackCtx *ctx, struct NetCtlMsg *msg)
{
    if (ctx->pendingShutdown != NULL)
        return NETCTL_ERR_INACTIVE; /* the stack is on its way out */
    if (ctx->created || ctx->pendingAdd != NULL)
        return NETCTL_ERR_EXISTS; /* one NIC in this stack (for now) */

    struct NetCtlIfConfig *nif = &msg->ncm_Config;
    nif->nif_Name[NETCTL_IFNAME_MAX - 1] = '\0';
    nif->nif_Device[NETCTL_DEV_MAX - 1] = '\0';
    nif->nif_Id[NETCTL_ID_MAX - 1] = '\0';
    if (nif->nif_Name[0] == '\0' || nif->nif_Device[0] == '\0')
        return NETCTL_ERR_PARAM;
    if (nif->nif_Type < NETCTL_TYPE_AUTO || nif->nif_Type > NETCTL_TYPE_SANA2)
        return NETCTL_ERR_PARAM;
    if ((nif->nif_Flags & NETCTL_IFF_HAS_MTU) && nif->nif_Mtu <= 0)
        return NETCTL_ERR_PARAM;
    if (!(nif->nif_Flags & NETCTL_IFF_DHCP) &&
        (nif->nif_Addr == 0 || !(nif->nif_Flags & NETCTL_IFF_HAS_MASK)))
        return NETCTL_ERR_PARAM; /* static needs at least ADDRESS + NETMASK */

    LONG aux = 0;
    LONG res = sb_if_up(ctx, nif, &aux);
    msg->ncm_Aux = aux;
    if (res != NETCTL_OK)
        return res;

    sb_netctl_stamp_start(ctx); /* IFQ_LastStart */

    /* Parked until operational — link up (static) or lease bound (DHCP) —
     * then answered by the tick poll, or recalled by CANCEL. The immediate
     * attempt covers a link that is already up (e.g. a quick re-add). */
    ctx->pendingAdd = msg;
    sb_netctl_try_complete_add(ctx);
    return SB_NETCTL_PARKED;
}

static LONG sb_netctl_cancel_add(struct SbStackCtx *ctx)
{
    if (ctx->pendingAdd == NULL)
        return NETCTL_ERR_NOTFOUND;

    /* Final readiness check kills the cancel-vs-completion race
     * deterministically: if the link/lease actually landed, the ADD wins and
     * reports success — the client accepts an OK ADD reply whoever answered
     * first. */
    if (sb_netctl_try_complete_add(ctx))
        return NETCTL_ERR_NOTFOUND;

    /* the interface stays added and keeps trying: a link or lease that
     * arrives late is strictly better than no interface */
    SB_LOG(NS_LOG_WARNING, "%s: no %s yet, AddNetInterface stopped waiting (the interface stays up)",
           sb_ctx_base(ctx)->nib_Name,
           (ctx->pendingAdd->ncm_Config.nif_Flags & NETCTL_IFF_DHCP) ? "DHCP lease" : "link");
    ctx->pendingAdd->ncm_Result = NETCTL_ERR_PENDING;
    sb_netctl_fill_outputs(ctx, ctx->pendingAdd);
    ReplyMsg(&ctx->pendingAdd->ncm_Msg);
    ctx->pendingAdd = NULL;
    return NETCTL_OK;
}

/* Sockets that pin the interface address: established/connected TCP, bound
 * listeners, and address-bound UDP. TIME_WAIT pcbs are deliberately excluded
 * (stack-owned, no application holds them) and wildcard binds survive an
 * interface removal by construction. Core lock held. */
static ULONG sb_netctl_bound_count(struct netif *nf)
{
    ULONG addr = ip4_addr_get_u32(netif_ip4_addr(nf));
    if (addr == 0)
        return 0;

    ULONG n = 0;
    for (struct tcp_pcb *p = tcp_active_pcbs; p != NULL; p = p->next)
    {
        if (ip4_addr_get_u32(ip_2_ip4(&p->local_ip)) == addr)
            n++;
    }
    for (struct tcp_pcb_listen *p = tcp_listen_pcbs.listen_pcbs; p != NULL; p = p->next)
    {
        if (ip4_addr_get_u32(ip_2_ip4(&p->local_ip)) == addr)
            n++;
    }
    for (struct udp_pcb *p = udp_pcbs; p != NULL; p = p->next)
    {
        if (ip4_addr_get_u32(ip_2_ip4(&p->local_ip)) == addr)
            n++;
    }
    return n;
}

static LONG sb_netctl_rem(struct SbStackCtx *ctx, struct NetCtlMsg *msg)
{
    if (ctx->pendingShutdown != NULL)
        return NETCTL_ERR_INACTIVE; /* teardown will take it down anyway */
    msg->ncm_Config.nif_Name[NETCTL_IFNAME_MAX - 1] = '\0';
    if (msg->ncm_Config.nif_Name[0] == '\0')
        return NETCTL_ERR_PARAM;

    netstack_lock();
    struct netif *nf = sb_if_find(msg->ncm_Config.nif_Name);
    BOOL match = ctx->created && nf == &sb_ctx_base(ctx)->nib_Netif;
    ULONG bound = match ? sb_netctl_bound_count(nf) : 0;
    netstack_unlock();

    if (!match)
        return NETCTL_ERR_NOTFOUND;
    if (msg->ncm_Force == 0 && bound > 0)
    {
        SB_LOG(NS_LOG_WARNING, "%s: %lu socket(s) still bound, RemoveNetInterface refused",
               sb_ctx_base(ctx)->nib_Name, bound);
        msg->ncm_Count = bound;
        return NETCTL_ERR_BUSY;
    }

    /* a DHCP wait cannot outlive its interface: the stack withdrew it */
    if (ctx->pendingAdd != NULL)
    {
        ctx->pendingAdd->ncm_Result = NETCTL_ERR_INACTIVE;
        ReplyMsg(&ctx->pendingAdd->ncm_Msg);
        ctx->pendingAdd = NULL;
    }

    SB_LOG(NS_LOG_NOTICE, "%s: interface removed%s", sb_ctx_base(ctx)->nib_Name,
           msg->ncm_Force != 0 ? " (forced)" : "");
    sb_stats_drain(ctx);
    sb_if_down(ctx);
    return NETCTL_OK;
}

/* Ask every registered opener to let go: Signal each with its configured
 * break mask (SBTC_BREAKMASK, default SIGBREAKF_CTRL_C) so blocking calls
 * return SB_EINTR and well-behaved clients exit. Under openLock. */
static void sb_netctl_nudge_openers(struct SocketBase *root)
{
    for (struct MinNode *n = root->openers.mlh_Head; n->mln_Succ != NULL;
         n = n->mln_Succ)
    {
        struct SocketBase *b = SB_OPENER_FROM_NODE(n);
        if (b->task != NULL && b->breakMask != 0)
            Signal(b->task, b->breakMask);
    }
}

static LONG sb_netctl_shutdown(struct SbStackCtx *ctx, struct NetCtlMsg *msg)
{
    if (ctx->pendingShutdown != NULL)
        return NETCTL_ERR_EXISTS; /* one is already in progress */

    struct SocketBase *root = ctx->root;
    ObtainSemaphore(&root->openLock);
    root->shuttingDown = TRUE; /* LibOpen refuses new clients from here on */
    sb_netctl_nudge_openers(root);
    SB_LOG(NS_LOG_NOTICE, "shutdown requested, asking %lu client(s) to quit", root->openCount);
    ReleaseSemaphore(&root->openLock);

    /* Parked until the last client closes (LibClose signals CTRL_E; the
     * stack task re-checks readiness every loop pass, so zero clients means
     * right now). The reply is the exiting task's very last act. */
    ctx->pendingShutdown = msg;
    return SB_NETCTL_PARKED;
}

static LONG sb_netctl_cancel_shutdown(struct SbStackCtx *ctx)
{
    if (ctx->pendingShutdown == NULL)
        return NETCTL_ERR_NOTFOUND;

    struct SocketBase *root = ctx->root;
    ObtainSemaphore(&root->openLock);
    root->shuttingDown = FALSE;
    ULONG clients = root->openCount;
    ReleaseSemaphore(&root->openLock);

    ctx->pendingShutdown->ncm_Result = NETCTL_ERR_ABORTED;
    ctx->pendingShutdown->ncm_Count = clients;
    ReplyMsg(&ctx->pendingShutdown->ncm_Msg);
    ctx->pendingShutdown = NULL;
    SB_LOG(NS_LOG_NOTICE, "shutdown cancelled, %lu client(s) remain", clients);
    return NETCTL_OK;
}

BOOL sb_netctl_shutdown_ready(struct SbStackCtx *ctx)
{
    if (ctx->pendingShutdown == NULL)
        return FALSE;

    struct SocketBase *root = ctx->root;
    ObtainSemaphore(&root->openLock);
    BOOL ready = root->openCount == 0;
    ReleaseSemaphore(&root->openLock);
    return ready;
}

void sb_netctl_nudge(struct SbStackCtx *ctx)
{
    if (ctx->pendingShutdown == NULL)
        return;

    /* re-signal once a second: a client that was mid-Wait() when the first
     * nudge landed may have consumed the signal without acting on it */
    struct SocketBase *root = ctx->root;
    ObtainSemaphore(&root->openLock);
    if (root->shuttingDown)
        sb_netctl_nudge_openers(root);
    ReleaseSemaphore(&root->openLock);
}

static LONG sb_netctl_handle(struct SbStackCtx *ctx, struct NetCtlMsg *msg)
{
    if (msg->ncm_Version != NETCTL_VERSION)
        return NETCTL_ERR_VERSION;

    switch (msg->ncm_Op)
    {
    case NETCTL_OP_ADD_IF:
        return sb_netctl_add(ctx, msg);
    case NETCTL_OP_CANCEL_ADD:
        return sb_netctl_cancel_add(ctx);
    case NETCTL_OP_REM_IF:
        return sb_netctl_rem(ctx, msg);
    case NETCTL_OP_SHUTDOWN:
        return sb_netctl_shutdown(ctx, msg);
    case NETCTL_OP_CANCEL_SHUTDOWN:
        return sb_netctl_cancel_shutdown(ctx);
    default:
        return NETCTL_ERR_OP;
    }
}

void sb_netctl_service(struct SbStackCtx *ctx)
{
    if (sbCtlPort == NULL)
        return;

    struct NetCtlMsg *msg;
    while ((msg = (struct NetCtlMsg *)GetMsg(sbCtlPort)) != NULL)
    {
        LONG res = sb_netctl_handle(ctx, msg);
        if (res == SB_NETCTL_PARKED)
            continue;
        msg->ncm_Result = res;
        ReplyMsg(&msg->ncm_Msg);
    }
}

void sb_netctl_tick(struct SbStackCtx *ctx)
{
    if (ctx->pendingAdd != NULL)
        sb_netctl_try_complete_add(ctx);
}
