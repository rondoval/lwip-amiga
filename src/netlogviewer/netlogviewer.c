/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * NetLogViewer — capture and display the log messages of bsdsocket.library
 * and its clients.
 *
 *   NetLogViewer [CX_POPKEY <key>] [CX_PRIORITY <n>] [CX_POPUP <YES|NO>]
 *
 * A Commodity (default hotkey "shift alt f8", Exchange Show/Hide) that opens
 * bsdsocket.library — starting the loopback-only stack when it is not
 * running, so start it before the first AddNetInterface to see the whole
 * bring-up — and installs the stack-wide log hook (SBTC_LOG_HOOK). Every
 * line the stack or any syslog() client emits arrives through the hook,
 * which runs on the emitter's task: it copies the line into a message and
 * posts it to this program's port; the window shows it with time, origin
 * and severity, keeps the last NLV_MAX_LINES, and can save the list.
 *
 * NetShutdown asks every library client to quit by signalling its break
 * mask; the viewer answers by clearing its hook, closing the library and
 * exiting. Ctrl-C does the same.
 */

#include "nlv.h"
#include "nlv_args.h"
#include "nlv_window.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <exec/memory.h>
#include <intuition/intuition.h>
#include <libraries/bsdsocket.h>
#include <libraries/commodities.h>
#include <utility/hooks.h>
#include <utility/tagitem.h>

#include <clib/alib_protos.h>
#include <proto/commodities.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/socket.h>

/* libnix: swap to a stack this large when the caller's is smaller —
 * ReAction layout and the ASL requester need more than a 4 KB Shell stack */
unsigned long __stack = 16384;

/* libnix.a has strlcpy, but under -mcrt=nix20 its <string.h> only declares it
 * when __NO_INLINE__ is set. Declare it (same signature) rather than grow yet
 * another bounded-copy helper. */
__stdargs size_t strlcpy(char *dst, const char *src, size_t size);

struct IntuitionBase *IntuitionBase;
struct Library *IconBase;
struct Library *CxBase;
struct Library *SocketBase;
BOOL nlvFromWb;

/* --- user diagnostics ----------------------------------------------------- */

static char reportBuf[512];

LONG nlv_request(struct Window *parent, const char *gadgets, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(reportBuf, sizeof(reportBuf), fmt, ap);
    va_end(ap);

    struct EasyStruct es = {
        sizeof(struct EasyStruct), 0, (UBYTE *)NLV_NAME, (UBYTE *)"%s", (UBYTE *)gadgets,
    };
    APTR earg[1] = {reportBuf};
    return EasyRequestArgs(parent, &es, NULL, (APTR)earg);
}

void nlv_report(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(reportBuf, sizeof(reportBuf), fmt, ap);
    va_end(ap);

    if (nlvFromWb && IntuitionBase != NULL)
    {
        struct EasyStruct es = {
            sizeof(struct EasyStruct), 0, (UBYTE *)NLV_NAME, (UBYTE *)"%s", (UBYTE *)"OK",
        };
        APTR earg[1] = {reportBuf};
        EasyRequestArgs(NULL, &es, NULL, (APTR)earg);
        return;
    }
    fprintf(stderr, NLV_NAME ": %s\n", reportBuf);
}

/* --- the log hook --------------------------------------------------------- */

struct NlvHookData
{
    struct MsgPort *port;
    ULONG pending; /* messages posted and not yet consumed (under Forbid) */
    ULONG dropped; /* lost to a full queue or a failed allocation */
};

static struct Hook logHook;
static struct NlvHookData hookData;

/* Runs on the emitter's task, under Forbid, possibly under the stack's core
 * lock: allocate, copy, post — nothing else. */
static VOID nlv_log_hook(struct Hook *hook asm("a0"), APTR reserved asm("a2"),
                         struct LogHookMessage *lhm asm("a1"))
{
    (void)reserved;
    struct NlvHookData *hd = hook->h_Data;

    if (lhm == NULL || lhm->lhm_Size < (LONG)sizeof(struct LogHookMessage) ||
        lhm->lhm_Message == NULL)
        return;
    if (hd->pending >= NLV_QUEUE_MAX)
    {
        hd->dropped++;
        return;
    }

    ULONG size = sizeof(struct NlvLogMsg) + NLV_TEXT_MAX + NLV_TAG_MAX;
    struct NlvLogMsg *m = AllocVec(size, MEMF_PUBLIC);
    if (m == NULL)
    {
        hd->dropped++;
        return;
    }
    m->nlm_Msg.mn_Node.ln_Type = NT_MESSAGE;
    m->nlm_Msg.mn_Node.ln_Pri = 0;
    m->nlm_Msg.mn_Node.ln_Name = NULL;
    m->nlm_Msg.mn_ReplyPort = NULL;
    m->nlm_Msg.mn_Length = (UWORD)size;
    m->nlm_Date = lhm->lhm_Date;
    m->nlm_Pri = lhm->lhm_Priority;
    m->nlm_Id = lhm->lhm_ID;
    strlcpy(m->nlm_Text, (const char *)lhm->lhm_Message, NLV_TEXT_MAX);
    if (lhm->lhm_Tag != NULL)
    {
        /* the tag lands after the (possibly truncated) text, in the same
         * allocation: measure what was written, not what strlcpy wanted */
        char *tag = m->nlm_Text + strlen(m->nlm_Text) + 1;
        strlcpy(tag, (const char *)lhm->lhm_Tag, NLV_TAG_MAX);
        m->nlm_Tag = tag;
    }
    else
        m->nlm_Tag = NULL;

    hd->pending++;
    PutMsg(hd->port, &m->nlm_Msg);
}

/* Everything queued, in one list update. Reports drops as a line of its own. */
static void nlv_drain(struct MsgPort *port)
{
    nlv_window_update_begin();
    struct NlvLogMsg *m;
    while ((m = (struct NlvLogMsg *)GetMsg(port)) != NULL)
    {
        nlv_window_update_add(m);
        FreeVec(m);
        Forbid();
        hookData.pending--;
        Permit();
    }

    Forbid();
    ULONG dropped = hookData.dropped;
    hookData.dropped = 0;
    Permit();
    if (dropped != 0)
    {
        struct NlvLogMsg *note = AllocVec(sizeof(struct NlvLogMsg) + 64, MEMF_PUBLIC | MEMF_CLEAR);
        if (note != NULL)
        {
            DateStamp(&note->nlm_Date);
            note->nlm_Pri = 4; /* LOG_WARNING */
            note->nlm_Tag = NLV_NAME;
            snprintf(note->nlm_Text, 64, "%lu message(s) lost while the viewer was busy",
                     (unsigned long)dropped);
            nlv_window_update_add(note);
            FreeVec(note);
        }
    }
    nlv_window_update_end();
}

/* --- the log hook, library side ------------------------------------------- */

static BOOL nlv_hook_install(void)
{
    logHook.h_Entry = (ULONG (*)())(APTR)nlv_log_hook;
    logHook.h_SubEntry = NULL;
    logHook.h_Data = &hookData;

    struct TagItem tags[2];
    tags[0].ti_Tag = SBTM_SETVAL(SBTC_LOG_HOOK);
    tags[0].ti_Data = (ULONG)&logHook;
    tags[1].ti_Tag = TAG_END;
    return SocketBaseTagList(tags) == 0;
}

/* Clear only if the installed hook is still ours (a later installer wins).
 * SocketBaseTagList returns after any in-flight hook call has finished. */
static void nlv_hook_remove(void)
{
    struct Hook *current = NULL;
    struct TagItem tags[2];
    tags[0].ti_Tag = SBTM_GETREF(SBTC_LOG_HOOK);
    tags[0].ti_Data = (ULONG)&current;
    tags[1].ti_Tag = TAG_END;
    if (SocketBaseTagList(tags) != 0 || current != &logHook)
        return;
    tags[0].ti_Tag = SBTM_SETVAL(SBTC_LOG_HOOK);
    tags[0].ti_Data = 0;
    SocketBaseTagList(tags);
}

/* --- the commodity -------------------------------------------------------- */

enum
{
    NLV_HOTKEY_ID = 1
};

static void nlv_flush_port(struct MsgPort *port, BOOL reply)
{
    struct Message *msg;
    while ((msg = GetMsg(port)) != NULL)
    {
        if (reply)
            ReplyMsg(msg);
        else
            FreeVec(msg);
    }
}

int main(int argc, char **argv)
{
    int rc = RETURN_FAIL;
    struct MsgPort *cxPort = NULL;
    struct MsgPort *logPort = NULL;
    CxObj *broker = NULL;
    BOOL hookInstalled = FALSE;
    BOOL windowReady = FALSE;

    nlvFromWb = argc == 0;
    IconBase = OpenLibrary((CONST_STRPTR) "icon.library", 36);
    IntuitionBase = (struct IntuitionBase *)OpenLibrary((CONST_STRPTR) "intuition.library", 36);
    if (IntuitionBase == NULL)
        goto out;

    struct NlvArgs args;
    if (!nlv_args_parse(argc, argv, &args))
    {
        rc = RETURN_ERROR;
        goto out;
    }

    CxBase = OpenLibrary((CONST_STRPTR) "commodities.library", 37);
    if (CxBase == NULL)
    {
        nlv_report("Could not open commodities.library V37.");
        goto out;
    }
    cxPort = CreateMsgPort();
    logPort = CreateMsgPort();
    if (cxPort == NULL || logPort == NULL)
    {
        nlv_report("Could not create internal message port.");
        goto out;
    }
    hookData.port = logPort;

    struct NewBroker nb;
    memset(&nb, 0, sizeof(nb));
    nb.nb_Version = NB_VERSION;
    nb.nb_Name = (STRPTR)NLV_NAME;
    nb.nb_Title = (STRPTR) "Network log viewer";
    nb.nb_Descr = (STRPTR) "Shows log messages";
    nb.nb_Unique = NBU_UNIQUE | NBU_NOTIFY;
    nb.nb_Flags = COF_SHOW_HIDE;
    nb.nb_Pri = (BYTE)args.priority;
    nb.nb_Port = cxPort;
    LONG cxErr = 0;
    broker = CxBroker(&nb, &cxErr);
    if (broker == NULL)
    {
        /* another NetLogViewer runs: NBU_NOTIFY just told it to show its
         * window, which is what the user wanted */
        rc = cxErr == CBERR_DUP ? RETURN_OK : RETURN_FAIL;
        if (cxErr != CBERR_DUP)
            nlv_report("Could not register with commodities.library (error %ld).", (long)cxErr);
        goto out;
    }
    CxObj *hotkey = HotKey((CONST_STRPTR)args.popKey, cxPort, NLV_HOTKEY_ID);
    if (hotkey == NULL && strcmp(args.popKey, NLV_DEFAULT_POPKEY) != 0)
    {
        nlv_report("'%s' is not a valid key description, using '%s'.", args.popKey,
                   NLV_DEFAULT_POPKEY);
        hotkey = HotKey((CONST_STRPTR)NLV_DEFAULT_POPKEY, cxPort, NLV_HOTKEY_ID);
    }
    if (hotkey != NULL)
        AttachCxObj(broker, hotkey);

    /* this is what boots the (loopback-only) stack when it is not running */
    SocketBase = OpenLibrary((CONST_STRPTR) "bsdsocket.library", 4);
    if (SocketBase == NULL)
    {
        nlv_report("Could not open bsdsocket.library V4 (the network stack is not "
                   "installed, or it is shutting down).");
        goto out;
    }
    if (!nlv_hook_install())
    {
        nlv_report("Could not register the log message hook interface (lwip-amiga or "
                   "Roadshow required).");
        goto out;
    }
    hookInstalled = TRUE;

    /* flagged before the call, not after: nlv_window_init() opens the class
     * libraries before it can fail, and nlv_window_exit() is safe the moment
     * its first statement (NewList) has run */
    windowReady = TRUE;
    if (!nlv_window_init())
        goto out;

    ActivateCxObj(broker, TRUE);
    if (args.popup)
        nlv_window_show();

    ULONG cxSig = 1UL << cxPort->mp_SigBit;
    ULONG logSig = 1UL << logPort->mp_SigBit;
    BOOL running = TRUE;
    while (running)
    {
        ULONG sigs = Wait(cxSig | logSig | nlv_window_sigmask() | SIGBREAKF_CTRL_C);

        /* Ctrl-C from the Shell, or NetShutdown's request to let go */
        if (sigs & SIGBREAKF_CTRL_C)
            break;

        if (sigs & cxSig)
        {
            CxMsg *cxm;
            while ((cxm = (CxMsg *)GetMsg(cxPort)) != NULL)
            {
                ULONG id = (ULONG)CxMsgID(cxm);
                ULONG type = CxMsgType(cxm);
                ReplyMsg((struct Message *)cxm);
                if (type == CXM_IEVENT && id == NLV_HOTKEY_ID)
                    nlv_window_show();
                else if (type == CXM_COMMAND)
                {
                    switch (id)
                    {
                    case CXCMD_APPEAR:
                    case CXCMD_UNIQUE:
                        nlv_window_show();
                        break;
                    case CXCMD_DISAPPEAR:
                        nlv_window_hide();
                        break;
                    case CXCMD_KILL:
                        running = FALSE;
                        break;
                    case CXCMD_DISABLE:
                        ActivateCxObj(broker, FALSE);
                        break;
                    case CXCMD_ENABLE:
                        ActivateCxObj(broker, TRUE);
                        break;
                    default:
                        break;
                    }
                }
            }
        }

        if (sigs & logSig)
            nlv_drain(logPort);

        if (nlv_window_sigmask() != 0)
        {
            switch (nlv_window_handle_input())
            {
            case NLV_ACT_HIDE:
                nlv_window_hide();
                break;
            case NLV_ACT_QUIT:
                running = FALSE;
                break;
            default:
                break;
            }
        }
    }
    rc = RETURN_OK;

out:
    /* order matters: no more hook calls, then whatever is still queued, then
     * the window, the broker, the ports, the libraries */
    if (hookInstalled)
        nlv_hook_remove();
    if (logPort != NULL)
        nlv_flush_port(logPort, FALSE);
    if (windowReady)
        nlv_window_exit();
    if (broker != NULL)
        DeleteCxObjAll(broker);
    if (cxPort != NULL)
    {
        nlv_flush_port(cxPort, TRUE);
        DeleteMsgPort(cxPort);
    }
    if (logPort != NULL)
        DeleteMsgPort(logPort);
    if (SocketBase != NULL)
        CloseLibrary(SocketBase);
    if (CxBase != NULL)
        CloseLibrary(CxBase);
    if (IntuitionBase != NULL)
        CloseLibrary((struct Library *)IntuitionBase);
    if (IconBase != NULL)
        CloseLibrary(IconBase);
    return rc;
}
