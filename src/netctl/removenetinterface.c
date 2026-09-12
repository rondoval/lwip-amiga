/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * RemoveNetInterface — remove a network interface from the running
 * lwip-amiga stack, Roadshow-style. Template: INTERFACE/A,FORCE/S,QUIET/S.
 *
 * Without FORCE the stack refuses when sockets are still bound to the
 * interface's address; FORCE removes it regardless (those connections are
 * aborted). Accepts both the Roadshow-style name ("genet") and lwIP's
 * short name ("nd0").
 *
 * Deliberately does NOT open bsdsocket.library — that would boot the stack
 * just to remove an interface from it. Talks straight to the control port.
 */

#include <stdio.h>
#include <string.h>

#include <dos/dos.h>
#include <exec/types.h>

#include <proto/dos.h>
#include <proto/exec.h>

#include "netctl_client.h"

#define ARG_TEMPLATE "INTERFACE/A,FORCE/S,QUIET/S"
enum
{
    ARG_INTERFACE,
    ARG_FORCE,
    ARG_QUIET,
    ARG_COUNT
};

int main(void)
{
    LONG args[ARG_COUNT];
    memset(args, 0, sizeof(args));
    struct RDArgs *rda = ReadArgs((CONST_STRPTR)ARG_TEMPLATE, args, NULL);
    if (rda == NULL)
    {
        PrintFault(IoErr(), (CONST_STRPTR) "RemoveNetInterface");
        return RETURN_ERROR;
    }
    const char *name = (const char *)args[ARG_INTERFACE];
    BOOL force = args[ARG_FORCE] != 0;
    BOOL quiet = args[ARG_QUIET] != 0;

    int rc;
    if (strlen(name) > NETCTL_IFNAME_MAX - 1)
    {
        if (!quiet)
            fprintf(stderr, "RemoveNetInterface: interface name is longer than "
                            "%d characters\n", NETCTL_IFNAME_MAX - 1);
        rc = RETURN_ERROR;
        goto out_args;
    }

    struct MsgPort *reply = CreateMsgPort();
    if (reply == NULL)
    {
        rc = RETURN_FAIL;
        goto out_args;
    }

    struct NetCtlMsg msg;
    netctl_msg_init(&msg, reply, NETCTL_OP_REM_IF);
    strcpy(msg.ncm_Config.nif_Name, name);
    msg.ncm_Force = force;

    if (!netctl_send(&msg))
    {
        if (!quiet)
            printf("The network is not running.\n");
        rc = RETURN_FAIL;
        goto out_port;
    }

    /* a remove is quick and cannot be recalled; the reply port is private,
     * so the one message that can arrive is our own coming back */
    WaitPort(reply);
    GetMsg(reply);

    switch (msg.ncm_Result)
    {
    case NETCTL_OK:
        if (!quiet)
            printf("interface '%s' removed.\n", name);
        rc = RETURN_OK;
        break;
    case NETCTL_ERR_BUSY:
        if (!quiet)
            fprintf(stderr, "RemoveNetInterface: interface '%s' is in use "
                            "(%lu socket(s)) - use FORCE to remove it anyway\n",
                    name, (unsigned long)msg.ncm_Count);
        rc = RETURN_ERROR;
        break;
    case NETCTL_ERR_NOTFOUND:
        if (!quiet)
            fprintf(stderr, "RemoveNetInterface: no interface '%s'\n", name);
        rc = RETURN_ERROR;
        break;
    case NETCTL_ERR_INACTIVE:
        if (!quiet)
            printf("the network is shutting down anyway.\n");
        rc = RETURN_WARN;
        break;
    default:
        if (!quiet)
            fprintf(stderr, "RemoveNetInterface: %s\n",
                    netctl_strerror(msg.ncm_Result));
        rc = RETURN_ERROR;
        break;
    }

out_port:
    DeleteMsgPort(reply);
out_args:
    FreeArgs(rda);
    if (quiet && rc > RETURN_WARN)
        rc = RETURN_WARN;
    return rc;
}
