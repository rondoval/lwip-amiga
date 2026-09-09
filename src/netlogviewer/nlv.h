/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * NetLogViewer — shared definitions.
 *
 * The program is a Commodity that installs the bsdsocket.library log hook
 * (SBTC_LOG_HOOK). The hook runs on whichever task emitted the line — the
 * stack task, a client, a driver task, possibly under the stack's core lock
 * — so it does one thing: copy the line into a message and PutMsg it to the
 * viewer's private port. Everything else (the list, the window, saving)
 * happens on the viewer's own task.
 */

#ifndef NLV_H
#define NLV_H

#include <dos/dos.h>
#include <exec/ports.h>
#include <exec/types.h>

#define NLV_NAME "NetLogViewer"

#define NLV_MAX_LINES 1000 /* list bound: the oldest lines are evicted */
#define NLV_QUEUE_MAX 512  /* lines in flight between the hook and the viewer */
#define NLV_TAG_MAX 64
#define NLV_TEXT_MAX 512
#define NLV_KEY_MAX 64

/* One captured line as the hook posts it: the text, then the tag copy, both
 * NUL-terminated, in the one allocation. */
struct NlvLogMsg
{
    struct Message nlm_Msg;
    struct DateStamp nlm_Date;
    LONG nlm_Pri;        /* LOG_EMERG..LOG_DEBUG */
    ULONG nlm_Id;        /* syslog facility, informational */
    const char *nlm_Tag; /* into nlm_Text storage, or NULL when the origin is unknown */
    char nlm_Text[1];
};

struct IntuitionBase;
struct Window;
extern struct IntuitionBase *IntuitionBase;
extern struct Library *IconBase; /* optional: Workbench tooltypes */
extern BOOL nlvFromWb;

/* Diagnostics to the user: a requester when started from Workbench, stderr
 * otherwise. nlv_request always uses a requester (parent may be NULL) and
 * returns the EasyRequest button number. */
void nlv_report(const char *fmt, ...);
LONG nlv_request(struct Window *parent, const char *gadgets, const char *fmt, ...);

#endif /* NLV_H */
