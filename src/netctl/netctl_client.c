/* SPDX-License-Identifier: BSD-3-Clause */
/* See netctl_client.h. */

#include <stdio.h>
#include <string.h>

#include <exec/nodes.h>

#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#else
#include <proto/exec.h>
#endif

#include "netctl_client.h"

void netctl_msg_init(struct NetCtlMsg *msg, struct MsgPort *reply, UWORD op)
{
    memset(msg, 0, sizeof(*msg));
    msg->ncm_Msg.mn_Node.ln_Type = NT_MESSAGE;
    msg->ncm_Msg.mn_Length = sizeof(*msg);
    msg->ncm_Msg.mn_ReplyPort = reply;
    msg->ncm_Version = NETCTL_VERSION;
    msg->ncm_Op = op;
    msg->ncm_Result = NETCTL_ERR_INACTIVE;
}

BOOL netctl_send(struct NetCtlMsg *msg)
{
    Forbid();
    struct MsgPort *port = FindPort((CONST_STRPTR)NETCTL_PORT_NAME);
    if (port != NULL)
        PutMsg(port, &msg->ncm_Msg);
    Permit();
    return port != NULL;
}

void netctl_drain(struct MsgPort *reply, ULONG count)
{
    while (count > 0)
    {
        WaitPort(reply);
        if (GetMsg(reply) != NULL)
            count--;
    }
}

void netctl_ntoa(ULONG addr, char *buf)
{
    sprintf(buf, "%lu.%lu.%lu.%lu", (addr >> 24) & 0xFF, (addr >> 16) & 0xFF,
            (addr >> 8) & 0xFF, addr & 0xFF);
}

const char *netctl_strerror(LONG result)
{
    switch (result)
    {
    case NETCTL_OK:
        return "no error";
    case NETCTL_ERR_VERSION:
        return "control protocol mismatch - command and bsdsocket.library differ";
    case NETCTL_ERR_OP:
        return "operation not supported by the stack";
    case NETCTL_ERR_PARAM:
        return "invalid configuration";
    case NETCTL_ERR_EXISTS:
        return "already present";
    case NETCTL_ERR_NOTFOUND:
        return "not found";
    case NETCTL_ERR_BUSY:
        return "in use";
    case NETCTL_ERR_DEVICE:
        return "network device error";
    case NETCTL_ERR_NOMEM:
        return "out of memory";
    case NETCTL_ERR_INACTIVE:
        return "the network stack is shutting down";
    case NETCTL_ERR_PENDING:
        return "not operational yet (no link or no DHCP lease)";
    case NETCTL_ERR_ABORTED:
        return "aborted";
    case NETCTL_ERR_HWTYPE:
        return "not a 48-bit Ethernet SANA-II device";
    default:
        return "unknown error";
    }
}
