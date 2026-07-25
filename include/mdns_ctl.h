/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * bsdsocket.library mDNS control port — the private protocol the `mdns`
 * command uses to inspect and change what the stack advertises.
 *
 * The stack task owns a public MsgPort named MDNSCTL_PORT_NAME while the
 * responder is active; a client fills a struct MdnsCtlMsg, PutMsg()s it and
 * waits for the reply.
 *
 * Boot-time services come from netstack.prefs (MDNS_SERVICE, see sb_config.h);
 * anything added through this port lives until the stack stops.
 *
 * Private to this component (library + tool built together): no compatibility
 * promise beyond mcm_Version, which the library rejects when it does not match.
 */

#ifndef MDNS_CTL_H
#define MDNS_CTL_H

#include <exec/ports.h>
#include <exec/types.h>

#define MDNSCTL_PORT_NAME "bsdsocket.mdns"
#define MDNSCTL_VERSION   1

#define MDNSCTL_TYPE_MAX     32 /* "_ssh._tcp" and the like */
#define MDNSCTL_NAME_MAX     64 /* instance name / advertised hostname */
#define MDNSCTL_MAX_SERVICES 4  /* == MDNS_MAX_SERVICES (checked in sb_mdns.c) */

/* mcm_Op */
#define MDNSCTL_OP_LIST     0 /* fill mcm_Host + mcm_List/mcm_Count */
#define MDNSCTL_OP_ADD      1 /* register mcm_Service, slot returned in it */
#define MDNSCTL_OP_DEL      2 /* unregister mcm_Service.mcs_Slot */
#define MDNSCTL_OP_ANNOUNCE 3 /* re-announce everything now */

/* mcm_Result */
#define MDNSCTL_OK           0
#define MDNSCTL_ERR_VERSION  (-1)
#define MDNSCTL_ERR_OP       (-2)
#define MDNSCTL_ERR_PARAM    (-3) /* malformed type, bad port or slot */
#define MDNSCTL_ERR_FULL     (-4) /* no free service slot */
#define MDNSCTL_ERR_INACTIVE (-5) /* responder not running on the interface */

struct MdnsCtlService
{
    char  mcs_Type[MDNSCTL_TYPE_MAX]; /* "_ssh._tcp" — base name + _tcp/_udp */
    char  mcs_Name[MDNSCTL_NAME_MAX]; /* instance name; "" = advertised hostname */
    UWORD mcs_Port;
    BYTE  mcs_Slot; /* responder slot; -1 when unset/free */
    UBYTE mcs_Pad;
};

struct MdnsCtlMsg
{
    struct Message mcm_Msg;
    UWORD          mcm_Version; /* MDNSCTL_VERSION */
    UWORD          mcm_Op;
    LONG           mcm_Result;

    struct MdnsCtlService mcm_Service; /* ADD input/output, DEL input */

    /* LIST output */
    char                  mcm_Host[MDNSCTL_NAME_MAX];
    UWORD                 mcm_Count;
    struct MdnsCtlService mcm_List[MDNSCTL_MAX_SERVICES];
};

#endif /* MDNS_CTL_H */
