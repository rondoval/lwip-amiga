/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * NetShutdown — stop the lwip-amiga TCP/IP stack, Roadshow-style.
 * Template: TIMEOUT/N,QUIET/S (timeout in seconds, default 5, minimum 1).
 *
 * Sends NETCTL_OP_SHUTDOWN to the stack's control port. The stack asks
 * every client (via its configured break signal) to let go; once the last
 * one closes the library, the stack tears everything down and the OK reply
 * is its final act — upon which this command expunges bsdsocket.library
 * from memory (RemLibrary). If clients hold out past TIMEOUT (or Ctrl-C),
 * the shutdown is recalled and the network keeps running.
 *
 * Deliberately does NOT open bsdsocket.library: opening it would boot the
 * stack just to stop it, and would hold the library open against its own
 * expunge.
 */

#include <stdio.h>
#include <string.h>

#include <devices/timer.h>
#include <dos/dos.h>
#include <exec/execbase.h>
#include <exec/types.h>

#include <proto/dos.h>
#include <proto/exec.h>

#include "netctl_client.h"

#define ARG_TEMPLATE "TIMEOUT/N,QUIET/S"
enum
{
    ARG_TIMEOUT,
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
        PrintFault(IoErr(), (CONST_STRPTR) "NetShutdown");
        return RETURN_ERROR;
    }
    BOOL quiet = args[ARG_QUIET] != 0;
    LONG timeout = 5;
    if (args[ARG_TIMEOUT] != 0)
        timeout = *(LONG *)args[ARG_TIMEOUT];
    FreeArgs(rda);

    if (timeout < 1)
    {
        if (!quiet)
            PrintFault(ERROR_BAD_NUMBER, (CONST_STRPTR) "NetShutdown");
        return RETURN_FAIL;
    }

    struct MsgPort *reply = CreateMsgPort();
    struct MsgPort *timerPort = CreateMsgPort();
    struct timerequest *treq =
        (struct timerequest *)CreateIORequest(timerPort, sizeof(struct timerequest));
    BOOL timerOpen = treq != NULL &&
                     OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_VBLANK, &treq->tr_node, 0) == 0;
    if (reply == NULL || !timerOpen)
    {
        if (!quiet)
            fprintf(stderr, "NetShutdown: out of memory\n");
        if (timerOpen)
            CloseDevice(&treq->tr_node);
        if (treq != NULL)
            DeleteIORequest(&treq->tr_node);
        if (timerPort != NULL)
            DeleteMsgPort(timerPort);
        if (reply != NULL)
            DeleteMsgPort(reply);
        return RETURN_FAIL;
    }

    int rc;
    struct NetCtlMsg msg, cancel;
    netctl_msg_init(&msg, reply, NETCTL_OP_SHUTDOWN);

    if (!netctl_send(&msg))
    {
        if (!quiet)
            printf("The network is not in use.\n");
        rc = RETURN_WARN;
        goto out;
    }

    if (!quiet)
    {
        printf("Waiting for network to shut down... ");
        fflush(stdout);
    }

    treq->tr_node.io_Command = TR_ADDREQUEST;
    treq->tr_time.tv_secs = (ULONG)timeout;
    treq->tr_time.tv_micro = 0;
    SendIO(&treq->tr_node);

    ULONG replySig = 1UL << reply->mp_SigBit;
    ULONG timerSig = 1UL << timerPort->mp_SigBit;
    BOOL broke = FALSE, timedOut = FALSE;
    for (;;)
    {
        ULONG sigs = Wait(replySig | timerSig | SIGBREAKF_CTRL_C);
        if (GetMsg(reply) != NULL)
            break; /* only the SHUTDOWN is in flight: that was its reply */

        if (sigs & SIGBREAKF_CTRL_C)
            broke = TRUE;
        if ((sigs & timerSig) && CheckIO(&treq->tr_node) != NULL)
            timedOut = TRUE;
        if (broke || timedOut)
        {
            /* Recall it. The shutdown may still win the race — the parked
             * message's result decides below. */
            netctl_msg_init(&cancel, reply, NETCTL_OP_CANCEL_SHUTDOWN);
            netctl_drain(reply, netctl_send(&cancel) ? 2 : 1);
            break;
        }
    }

    AbortIO(&treq->tr_node);
    WaitIO(&treq->tr_node);

    switch (msg.ncm_Result)
    {
    case NETCTL_OK:
        /* the stack task is gone; drop the library from memory so the next
         * OpenLibrary starts from a clean load */
        Forbid();
        {
            struct Library *lib = (struct Library *)FindName(
                &SysBase->LibList, (CONST_STRPTR) "bsdsocket.library");
            if (lib != NULL)
                RemLibrary(lib);
        }
        Permit();
        if (!quiet)
            printf("shutdown finished.\n");
        rc = RETURN_OK;
        break;

    case NETCTL_ERR_ABORTED:
        if (!quiet)
            printf("%s; the network keeps running (%lu client(s) still active).\n",
                   broke ? "stopped waiting" : "timeout",
                   (unsigned long)msg.ncm_Count);
        rc = RETURN_WARN;
        break;

    case NETCTL_ERR_EXISTS:
        if (!quiet)
            printf("a shutdown is already in progress.\n");
        rc = RETURN_WARN;
        break;

    default:
        if (!quiet)
            printf("failed: %s\n", netctl_strerror(msg.ncm_Result));
        rc = RETURN_FAIL;
        break;
    }

out:
    CloseDevice(&treq->tr_node);
    DeleteIORequest(&treq->tr_node);
    DeleteMsgPort(timerPort);
    DeleteMsgPort(reply);
    if (quiet && rc > RETURN_WARN)
        rc = RETURN_WARN;
    return rc;
}
