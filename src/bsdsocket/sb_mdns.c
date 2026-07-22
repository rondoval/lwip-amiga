/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * mDNS / DNS-SD presence.
 *
 * lwIP's responder answers for <hostname>.local and for the DNS-SD services
 * we register with it; it joins 224.0.0.251. With
 * LWIP_NETIF_EXT_STATUS_CALLBACK the responder re-probes and re-announces
 * itself across link and DHCP-address changes, so there is nothing to drive
 * from here beyond setup and teardown.
 *
 * Services come from two places: netstack.prefs (MDNS_SERVICE, the boot set)
 * and the public control port (mdns_ctl.h) — for the `mdns` command.
 * Control registrations last until the stack stops.
 *
 * Everything here runs on the stack task. lwIP raw-API calls take the core
 * lock; the port bookkeeping deliberately does not hold it.
 */

#include "sb_base.h"

#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#else
#include <proto/exec.h>
#endif

#include <debug.h>
#include <mdns_ctl.h>

#include <lwip/apps/mdns.h>
#include <lwip/netif.h>

#include "netstack.h"
#include "sb_mdns.h"

#if MDNS_MAX_SERVICES != MDNSCTL_MAX_SERVICES
#error "mdns_ctl.h and lwipopts.h disagree on the service-slot count"
#endif

/* DNS wire limit for one label (RFC 1035); lwIP rejects anything longer. */
#define SB_MDNS_LABEL_MAX 63

static struct MsgPort *sbMdnsPort;
static struct netif *sbMdnsNetif;
static char sbMdnsHost[MDNSCTL_NAME_MAX];
static struct MdnsCtlService sbMdnsServices[MDNSCTL_MAX_SERVICES];
/* mdns_resp_init() allocates a pcb and a netif client-data id, so it must run
 * exactly once. The stack task starts once per library load (LibOpen ->
 * sb_stack_start, LibExpunge -> unload), but the guard makes that explicit. */
static BOOL sbMdnsInited;

/* Host part of a name, reduced to one DNS label: everything up to the first
 * dot, with anything outside [A-Za-z0-9-] folded to '-'. */
static void sb_mdns_label(char *dst, ULONG max, const char *src)
{
    ULONG n = 0;
    if (max > SB_MDNS_LABEL_MAX + 1)
        max = SB_MDNS_LABEL_MAX + 1;
    while (src[n] != '\0' && src[n] != '.' && n < max - 1)
    {
        char c = src[n];
        BOOL ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-';
        dst[n] = ok ? c : '-';
        n++;
    }
    dst[n] = '\0';
    if (n == 0)
        strlcpy(dst, "amiga", max);
}

/* "_ssh._tcp" -> base "_ssh" + DNSSD_PROTO_TCP. A missing leading underscore
 * is added, since that is the common way to get this wrong. */
static BOOL sb_mdns_split_type(const char *type, char *base, ULONG baseMax,
                               enum mdns_sd_proto *proto)
{
    ULONG len = strlen(type);
    if (len < 6) /* shortest possible: "_x._tcp" minus the underscore */
        return FALSE;

    const char *tail = type + len - 5;
    if (_Stricmp((CONST_STRPTR)tail, (CONST_STRPTR) "._tcp") == 0)
        *proto = DNSSD_PROTO_TCP;
    else if (_Stricmp((CONST_STRPTR)tail, (CONST_STRPTR) "._udp") == 0)
        *proto = DNSSD_PROTO_UDP;
    else
        return FALSE;

    ULONG baseLen = len - 5;
    ULONG out = 0;
    if (type[0] != '_')
    {
        if (baseMax < 2)
            return FALSE;
        base[out++] = '_';
    }
    if (baseLen + out > baseMax - 1 || baseLen + out > SB_MDNS_LABEL_MAX)
        return FALSE;
    for (ULONG i = 0; i < baseLen; i++)
        base[out++] = type[i];
    base[out] = '\0';
    return out > 1; /* "_" alone is not a service type */
}

/* Register one service and remember it for LIST. Returns an MDNSCTL_* result;
 * the assigned slot lands in the caller's record. */
static LONG sb_mdns_register(const char *type, const char *name, UWORD port,
                             BYTE *slotOut)
{
    char base[MDNSCTL_TYPE_MAX];
    enum mdns_sd_proto proto;

    if (sbMdnsNetif == NULL)
        return MDNSCTL_ERR_INACTIVE;
    if (port == 0 || !sb_mdns_split_type(type, base, sizeof(base), &proto))
        return MDNSCTL_ERR_PARAM;

    ULONG free_ = MDNSCTL_MAX_SERVICES;
    for (ULONG i = 0; i < MDNSCTL_MAX_SERVICES; i++)
    {
        if (sbMdnsServices[i].mcs_Slot < 0)
        {
            free_ = i;
            break;
        }
    }
    if (free_ == MDNSCTL_MAX_SERVICES)
        return MDNSCTL_ERR_FULL;

    const char *instance = (name != NULL && name[0] != '\0') ? name : sbMdnsHost;
    if (strlen(instance) > SB_MDNS_LABEL_MAX)
        return MDNSCTL_ERR_PARAM;

    netstack_lock();
    s8_t slot = mdns_resp_add_service(sbMdnsNetif, instance, base, proto, port, NULL, NULL);
    netstack_unlock();
    if (slot < 0)
    {
        Kprintf("[bsdsocket] mDNS: %s on port %lu rejected (%ld)\n", type, (ULONG)port,
                (LONG)slot);
        return slot == ERR_MEM ? MDNSCTL_ERR_FULL : MDNSCTL_ERR_PARAM;
    }

    struct MdnsCtlService *rec = &sbMdnsServices[free_];
    strlcpy(rec->mcs_Type, type, sizeof(rec->mcs_Type));
    strlcpy(rec->mcs_Name, instance, sizeof(rec->mcs_Name));
    rec->mcs_Port = port;
    rec->mcs_Slot = (BYTE)slot;
    if (slotOut != NULL)
        *slotOut = (BYTE)slot;

    Kprintf("[bsdsocket] mDNS: advertising %s %s port %lu (slot %ld)\n", rec->mcs_Name,
            rec->mcs_Type, (ULONG)port, (LONG)slot);
    return MDNSCTL_OK;
}

static LONG sb_mdns_unregister(BYTE slot)
{
    if (sbMdnsNetif == NULL)
        return MDNSCTL_ERR_INACTIVE;

    for (ULONG i = 0; i < MDNSCTL_MAX_SERVICES; i++)
    {
        struct MdnsCtlService *rec = &sbMdnsServices[i];
        if (rec->mcs_Slot != slot)
            continue;

        netstack_lock();
        err_t err = mdns_resp_del_service(sbMdnsNetif, (u8_t)slot);
        netstack_unlock();
        if (err != ERR_OK)
            return MDNSCTL_ERR_PARAM;

        Kprintf("[bsdsocket] mDNS: withdrew %s (slot %ld)\n", rec->mcs_Type, (LONG)slot);
        rec->mcs_Type[0] = '\0';
        rec->mcs_Name[0] = '\0';
        rec->mcs_Port = 0;
        rec->mcs_Slot = -1;
        return MDNSCTL_OK;
    }
    return MDNSCTL_ERR_PARAM;
}

void sb_mdns_start(struct netif *nif, const struct SbNetConfig *cfg)
{
    if (!cfg->cfg_Mdns || sbMdnsNetif != NULL)
        return;

    sb_mdns_label(sbMdnsHost, sizeof(sbMdnsHost), cfg->cfg_Hostname);
    for (ULONG i = 0; i < MDNSCTL_MAX_SERVICES; i++)
        sbMdnsServices[i].mcs_Slot = -1;

    netstack_lock();
    if (!sbMdnsInited)
    {
        mdns_resp_init();
        sbMdnsInited = TRUE;
    }
    err_t err = mdns_resp_add_netif(nif, sbMdnsHost);
    netstack_unlock();
    if (err != ERR_OK)
    {
        Kprintf("[bsdsocket] mDNS: responder start failed (%ld)\n", (LONG)err);
        return;
    }
    sbMdnsNetif = nif;
    Kprintf("[bsdsocket] mDNS: advertising %s.local\n", sbMdnsHost);

    for (ULONG i = 0; i < cfg->cfg_NumMdnsServices; i++)
    {
        const struct SbCfgMdnsService *s = &cfg->cfg_MdnsServices[i];
        if (sb_mdns_register(s->type, s->name, s->port, NULL) != MDNSCTL_OK)
            Kprintf("[bsdsocket] netstack.prefs: MDNS_SERVICE '%s' not advertised\n",
                    s->type);
    }

    sbMdnsPort = CreateMsgPort();
    if (sbMdnsPort == NULL)
    {
        Kprintf("[bsdsocket] mDNS: no control port (out of memory)\n");
        return;
    }
    sbMdnsPort->mp_Node.ln_Name = (char *)MDNSCTL_PORT_NAME;
    sbMdnsPort->mp_Node.ln_Pri = 0;
    AddPort(sbMdnsPort);
}

void sb_mdns_stop(void)
{
    if (sbMdnsPort != NULL)
    {
        /* Unpublish first: a client's FindPort+PutMsg runs under its own
         * Forbid(), so after this it either never saw the port or its message
         * is already queued — and the drain below answers those. */
        Forbid();
        RemPort(sbMdnsPort);
        Permit();

        struct MdnsCtlMsg *msg;
        while ((msg = (struct MdnsCtlMsg *)GetMsg(sbMdnsPort)) != NULL)
        {
            msg->mcm_Result = MDNSCTL_ERR_INACTIVE;
            ReplyMsg(&msg->mcm_Msg);
        }
        DeleteMsgPort(sbMdnsPort);
        sbMdnsPort = NULL;
    }

    if (sbMdnsNetif != NULL)
    {
        netstack_lock();
        mdns_resp_remove_netif(sbMdnsNetif);
        netstack_unlock();
        sbMdnsNetif = NULL;
    }
}

ULONG sb_mdns_sigmask(void)
{
    return sbMdnsPort != NULL ? (1UL << sbMdnsPort->mp_SigBit) : 0;
}

static LONG sb_mdns_handle(struct MdnsCtlMsg *msg)
{
    if (msg->mcm_Version != MDNSCTL_VERSION)
        return MDNSCTL_ERR_VERSION;
    if (sbMdnsNetif == NULL)
        return MDNSCTL_ERR_INACTIVE;

    switch (msg->mcm_Op)
    {
    case MDNSCTL_OP_LIST:
        strlcpy(msg->mcm_Host, sbMdnsHost, sizeof(msg->mcm_Host));
        msg->mcm_Count = 0;
        for (ULONG i = 0; i < MDNSCTL_MAX_SERVICES; i++)
        {
            if (sbMdnsServices[i].mcs_Slot >= 0)
                msg->mcm_List[msg->mcm_Count++] = sbMdnsServices[i];
        }
        return MDNSCTL_OK;

    case MDNSCTL_OP_ADD:
        msg->mcm_Service.mcs_Type[MDNSCTL_TYPE_MAX - 1] = '\0';
        msg->mcm_Service.mcs_Name[MDNSCTL_NAME_MAX - 1] = '\0';
        return sb_mdns_register(msg->mcm_Service.mcs_Type, msg->mcm_Service.mcs_Name,
                                msg->mcm_Service.mcs_Port, &msg->mcm_Service.mcs_Slot);

    case MDNSCTL_OP_DEL:
        return sb_mdns_unregister(msg->mcm_Service.mcs_Slot);

    case MDNSCTL_OP_ANNOUNCE:
        netstack_lock();
        mdns_resp_announce(sbMdnsNetif);
        netstack_unlock();
        return MDNSCTL_OK;

    default:
        return MDNSCTL_ERR_OP;
    }
}

void sb_mdns_service(void)
{
    if (sbMdnsPort == NULL)
        return;

    struct MdnsCtlMsg *msg;
    while ((msg = (struct MdnsCtlMsg *)GetMsg(sbMdnsPort)) != NULL)
    {
        msg->mcm_Result = sb_mdns_handle(msg);
        ReplyMsg(&msg->mcm_Msg);
    }
}
