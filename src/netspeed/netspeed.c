/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * NetSpeed — network throughput monitor for lwip-amiga.
 *
 * A ReAction window showing the current, average and maximum throughput of
 * one network interface (or the whole stack) in both directions, the link
 * speed, and a scrolling graph. Samples come from bsdsocket.library's
 * interface query API once a second. No arguments: the interface, graph
 * column time, average window, units and graph scale are set from the
 * Settings menu and kept in ENV:NetSpeed.prefs by Settings » Save.
 *
 * NetShutdown asks every library client to quit by signalling its break
 * mask; the tool answers by closing the library and exiting. Ctrl-C does
 * the same.
 */

#include "netspeed.h"
#include "nsp_graph.h"
#include "nsp_menu.h"
#include "nsp_prefs.h"
#include "nsp_sample.h"
#include "nsp_window.h"

#include <stdarg.h>
#include <stdio.h>

#include <devices/timer.h>
#include <exec/ports.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <libraries/bsdsocket.h>
#include <utility/tagitem.h>

#include <clib/alib_protos.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/socket.h>
#include <proto/timer.h>

/* libnix: swap to a stack this large when the caller's is smaller */
unsigned long __stack = 16384;

__attribute__((used)) static const char verstag[] =
    "\0$VER: " NSP_NAME " " TOOL_VERSION " " TOOL_DATE;

struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;
struct Library *IconBase;
struct Library *SocketBase;
struct Device *TimerBase;
BOOL nspFromWb;

/* --- user diagnostics ----------------------------------------------------- */

static char reportBuf[512];

/* The message is formatted here and passed as an argument, so its own text
 * can never be taken for a format. */
LONG nsp_request(struct Window *parent, const char *gadgets, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(reportBuf, sizeof(reportBuf), fmt, ap);
    va_end(ap);

    struct EasyStruct es = {
        sizeof(struct EasyStruct), 0, (UBYTE *)NSP_NAME, (UBYTE *)"%s", (UBYTE *)gadgets,
    };
    APTR earg[1] = {reportBuf};
    return EasyRequestArgs(parent, &es, NULL, (APTR)earg);
}

/* A requester needs Intuition, so before it is open the Shell path is used
 * even from Workbench. */
void nsp_report(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(reportBuf, sizeof(reportBuf), fmt, ap);
    va_end(ap);

    if (nspFromWb && IntuitionBase != NULL)
    {
        struct EasyStruct es = {
            sizeof(struct EasyStruct), 0, (UBYTE *)NSP_NAME, (UBYTE *)"%s", (UBYTE *)"OK",
        };
        APTR earg[1] = {reportBuf};
        EasyRequestArgs(NULL, &es, NULL, (APTR)earg);
        return;
    }
    fprintf(stderr, NSP_NAME ": %s\n", reportBuf);
}

/* --- the tick timer ------------------------------------------------------- */

struct NspTimer
{
    struct MsgPort *port;
    struct timerequest *req;
    BOOL open;
    BOOL armed;
    ULONG sig;
};

/* timer.device UNIT_VBLANK; also sets TimerBase for ReadEClock(). */
static BOOL nsp_timer_init(struct NspTimer *t)
{
    t->port = CreateMsgPort();
    t->req = (struct timerequest *)CreateIORequest(t->port, sizeof(struct timerequest));
    if (t->req == NULL ||
        OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_VBLANK, &t->req->tr_node, 0) != 0)
        return FALSE;
    t->open = TRUE;
    t->sig = 1UL << t->port->mp_SigBit;
    TimerBase = t->req->tr_node.io_Device;
    return TRUE;
}

/* One request in flight at a time: a second arm is a no-op. */
static void nsp_timer_arm(struct NspTimer *t, LONG secs)
{
    if (t->armed)
        return;
    t->req->tr_node.io_Command = TR_ADDREQUEST;
    t->req->tr_time.tv_secs = (ULONG)secs;
    t->req->tr_time.tv_micro = 0;
    SendIO(&t->req->tr_node);
    t->armed = TRUE;
}

/* Collect the reply; FALSE when the signal was not the timer's. */
static BOOL nsp_timer_fired(struct NspTimer *t)
{
    if (GetMsg(t->port) == NULL)
        return FALSE;
    t->armed = FALSE;
    return TRUE;
}

/* Safe on a failed or never-opened timer, so the exit path is unconditional. */
static void nsp_timer_exit(struct NspTimer *t)
{
    if (t->open)
    {
        if (t->armed)
        {
            if (!CheckIO(&t->req->tr_node))
                AbortIO(&t->req->tr_node);
            WaitIO(&t->req->tr_node);
        }
        CloseDevice(&t->req->tr_node);
    }
    if (t->req != NULL)
        DeleteIORequest(&t->req->tr_node);
    if (t->port != NULL)
        DeleteMsgPort(t->port);
    t->open = FALSE;
    t->armed = FALSE;
    t->req = NULL;
    t->port = NULL;
}

/* --- the context ---------------------------------------------------------- */

struct NspContext
{
    struct NspSettings settings;
    struct NspSampler sampler;
    struct NspGraph graph;
    struct NspWindow window;
    struct NspTimer timer;
};

/* The menu strip follows the interface set and the settings. */
static void nsp_sync_menu(struct NspContext *c)
{
    nsp_menu_sync(&c->window.menu, c->window.obj, &c->sampler.ifaces, &c->settings);
}

/* Push the settings into the model, then bring the readouts and the graph
 * up to date. Idempotent: runs after every input drain, every tick and at
 * startup, so no caller has to know which setting affects what. */
static void nsp_apply(struct NspContext *c)
{
    nsp_sample_set_average(&c->sampler, NSP_VALUE(&c->settings, NSP_SET_AVERAGE));
    nsp_graph_set_column(&c->graph, NSP_VALUE(&c->settings, NSP_SET_GRAPH));
    nsp_graph_set_scale(&c->graph, NSP_VALUE(&c->settings, NSP_SET_SCALE) != 0,
                        c->sampler.linkBytes);
    /* sampling never waits for the window: iconified or busy, the ring keeps
     * the history and the next drawable call catches up */
    if (nsp_graph_draw(&c->graph, c->window.win))
        nsp_window_refresh(&c->window, &c->sampler, &c->settings);
}

/* Point everything at a source: the sampler seeds now (so the next tick
 * already yields a rate), the graph forgets its history, the menu moves its
 * checkmark. An interface the stack does not know falls back to the whole
 * stack, which cannot fail. */
static void nsp_select_source(struct NspContext *c, const char *name)
{
    if (name != c->settings.interface)
        strlcpy(c->settings.interface, name, NSP_NAME_MAX);
    if (!nsp_sample_select(&c->sampler, c->settings.interface))
    {
        c->settings.interface[0] = '\0';
        nsp_sample_select(&c->sampler, "");
    }
    nsp_graph_reset(&c->graph);
    nsp_sync_menu(c);
}

/* Project » Reset statistics: the same source, statistics from now. */
static void nsp_reset_stats(struct NspContext *c)
{
    nsp_sample_reset(&c->sampler);
    nsp_graph_reset(&c->graph);
}

/* Settings » Save: the current settings plus the window box, to ENV: and
 * ENVARC:. Nothing is written at any other time. */
static void nsp_save(struct NspContext *c)
{
    struct IBox box;
    if (nsp_window_get_box(&c->window, &box))
    {
        c->settings.box = box;
        c->settings.haveBox = TRUE;
    }
    const char *path = NULL;
    LONG err = 0;
    if (!nsp_prefs_save(&c->settings, &path, &err))
    {
        char fault[80];
        Fault(err, NULL, (STRPTR)fault, sizeof(fault));
        nsp_request(c->window.win, "OK", "Could not save the settings to\n%s\n%s", path, fault);
    }
}

/* The timer fired: one sample, then the model and the window follow. */
static void nsp_tick(struct NspContext *c)
{
    ULONG r = nsp_sample_tick(&c->sampler, c->settings.interface);
    if (r & NSP_TICK_FAILED)
        nsp_select_source(c, ""); /* the interface went away */
    else
    {
        if (r & NSP_TICK_IFACES)
            nsp_sync_menu(c);
        if (r & NSP_TICK_RATE)
            nsp_graph_push(&c->graph, c->sampler.curRx, c->sampler.curTx);
    }
    nsp_apply(c);
    nsp_timer_arm(&c->timer, NSP_TICK_SECS);
}

/* --- startup -------------------------------------------------------------- */

/* Roadshow and lwip-amiga answer this; a stack without it has no counters. */
static BOOL nsp_have_interface_api(void)
{
    LONG have = 0;
    struct TagItem tags[2];
    tags[0].ti_Tag = SBTM_GETREF(SBTC_HAVE_INTERFACE_API);
    tags[0].ti_Data = (ULONG)&have;
    tags[1].ti_Tag = TAG_END;
    return SocketBaseTagList(tags) == 0 && have;
}

/* The default public screen's width: the history is one column per pixel. */
static ULONG nsp_screen_width(void)
{
    ULONG width = 640;
    struct Screen *scr = LockPubScreen(NULL);
    if (scr != NULL)
    {
        width = (ULONG)scr->Width;
        UnlockPubScreen(NULL, scr);
    }
    return width;
}

/* Libraries, stack, settings, model, window, then the event loop. The
 * context is static and zeroed, and every *_exit() is safe on its zeroed
 * sub-struct, so the exit path runs unconditionally. */
int main(int argc, char **argv)
{
    (void)argv;
    int rc = RETURN_FAIL;
    static struct NspContext ctx;

    nspFromWb = argc == 0;
    IntuitionBase = (struct IntuitionBase *)OpenLibrary((CONST_STRPTR) "intuition.library", 36);
    GfxBase = (struct GfxBase *)OpenLibrary((CONST_STRPTR) "graphics.library", 39);
    IconBase = OpenLibrary((CONST_STRPTR) "icon.library", 36); /* optional: the AppIcon */
    if (IntuitionBase == NULL || GfxBase == NULL)
        goto out;

    if (!nsp_timer_init(&ctx.timer))
    {
        nsp_report("Could not open timer.device.");
        goto out;
    }
    struct EClockVal ev;
    ULONG eclockFreq = ReadEClock(&ev);

    /* this is what boots the (loopback-only) stack when it is not running */
    SocketBase = OpenLibrary((CONST_STRPTR) "bsdsocket.library", 4);
    if (SocketBase == NULL)
    {
        nsp_report("Could not open bsdsocket.library V4 (the network stack is not "
                   "installed, or it is shutting down).");
        goto out;
    }
    if (!nsp_have_interface_api())
    {
        nsp_report("This network stack has no interface query API (lwip-amiga or "
                   "Roadshow required).");
        goto out;
    }

    nsp_prefs_load(&ctx.settings);
    nsp_sample_init(&ctx.sampler, eclockFreq);
    if (!nsp_graph_init(&ctx.graph, nsp_screen_width()))
    {
        nsp_report("Not enough memory for the sample history.");
        goto out;
    }
    if (!nsp_window_init(&ctx.window, &ctx.settings, &ctx.sampler.ifaces, &ctx.graph.hook))
        goto out;
    if (!nsp_window_open(&ctx.window))
        goto out;

    /* a saved interface the stack no longer has falls back to all */
    nsp_select_source(&ctx, ctx.settings.interface);
    nsp_apply(&ctx);
    nsp_timer_arm(&ctx.timer, NSP_TICK_SECS);

    for (;;)
    {
        /* the window's bit changes across iconify and reopen */
        ULONG winSig = nsp_window_sigmask(&ctx.window);
        ULONG sigs = Wait(ctx.timer.sig | winSig | SIGBREAKF_CTRL_C);

        /* Ctrl-C from the Shell, or NetShutdown's request to let go */
        if (sigs & SIGBREAKF_CTRL_C)
            break;

        if ((sigs & ctx.timer.sig) && nsp_timer_fired(&ctx.timer))
            nsp_tick(&ctx);

        if (sigs & winSig)
        {
            ULONG evs = nsp_window_handle_input(&ctx.window, &ctx.settings);
            if (evs & NSP_EV_QUIT)
                break;
            if (evs & NSP_EV_SOURCE)
                nsp_select_source(&ctx, ctx.settings.interface);
            if (evs & NSP_EV_RESET)
                nsp_reset_stats(&ctx);
            if (evs & NSP_EV_SAVE)
                nsp_save(&ctx);
            nsp_apply(&ctx);
            /* last: Save above must have seen the open window */
            if (evs & NSP_EV_ICONIFY)
                nsp_window_iconify(&ctx.window);
        }
    }
    rc = RETURN_OK;

out:
    /* order matters: no more ticks, then the window (its hook reads the
     * graph), then the history, then the libraries */
    nsp_timer_exit(&ctx.timer);
    nsp_window_exit(&ctx.window);
    nsp_graph_exit(&ctx.graph);
    if (SocketBase != NULL)
        CloseLibrary(SocketBase);
    if (IconBase != NULL)
        CloseLibrary(IconBase);
    if (GfxBase != NULL)
        CloseLibrary((struct Library *)GfxBase);
    if (IntuitionBase != NULL)
        CloseLibrary((struct Library *)IntuitionBase);
    return rc;
}
