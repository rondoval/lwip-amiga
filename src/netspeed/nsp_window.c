/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * NetSpeed window — see nsp_window.h.
 */

#include "nsp_window.h"

#include <stdio.h>
#include <string.h>

#include <classes/window.h>
#include <gadgets/button.h>
#include <gadgets/layout.h>
#include <gadgets/space.h>
#include <images/bevel.h>
#include <images/label.h>
#include <intuition/gadgetclass.h>
#include <intuition/intuition.h>
#include <libraries/bsdsocket.h>
#include <reaction/reaction_macros.h> /* the RA_* window.class method wrappers */
#include <workbench/workbench.h>

#include <clib/alib_protos.h>
#include <proto/button.h>
#include <proto/exec.h>
#include <proto/icon.h>
#include <proto/intuition.h>
#include <proto/label.h>
#include <proto/layout.h>
#include <proto/space.h>
#include <proto/window.h>

struct Library *WindowBase;
struct Library *LayoutBase;
struct Library *ButtonBase;
struct Library *SpaceBase;
struct Library *LabelBase;

#define NSP_DEFAULT_WIDTH 520
#define NSP_DEFAULT_HEIGHT 300
#define NSP_ROW_MAX 3 /* labelled fields in one row */

/* --- readouts ------------------------------------------------------------- */

/* "12.3 MB/s" / "98.6 Mb/s": one decimal, decimal prefixes (1000) so the
 * numbers line up with a "1000 Mb/s" link; "-" while there is no rate. */
static void nsp_window_format_rate(char *buf, size_t len, ULONG bytesPerSec, BOOL bits, BOOL valid)
{
    if (!valid)
    {
        strlcpy(buf, "-", len);
        return;
    }
    static const struct
    {
        unsigned long long div;
        char prefix;
    } scale[] = {{1000000000ULL, 'G'}, {1000000ULL, 'M'}, {1000ULL, 'k'}};
    unsigned long long v = bits ? (unsigned long long)bytesPerSec * 8 : bytesPerSec;
    char unit = bits ? 'b' : 'B';
    for (ULONG i = 0; i < sizeof(scale) / sizeof(scale[0]); i++)
        if (v >= scale[i].div)
        {
            snprintf(buf, len, "%lu.%lu %c%c/s", (unsigned long)(v / scale[i].div),
                     (unsigned long)(v % scale[i].div / (scale[i].div / 10)), scale[i].prefix,
                     unit);
            return;
        }
    snprintf(buf, len, "%lu %c/s", (unsigned long)v, unit);
}

/* "genet, 1000 Mb/s, up" / "All interfaces, down" / "All interfaces" */
static void nsp_window_format_source(char *buf, size_t len, const struct NspSampler *smp,
                                     const struct NspSettings *s)
{
    const char *name = s->interface[0] != '\0' ? s->interface : "All interfaces";
    if (!smp->haveLink)
        snprintf(buf, len, "%s", name);
    else if (smp->state != SM_Up)
        snprintf(buf, len, "%s, down", name);
    else if (smp->bps >= 1000000)
        snprintf(buf, len, "%s, %lu Mb/s, up", name, (unsigned long)(smp->bps / 1000000));
    else if (smp->bps > 0)
        snprintf(buf, len, "%s, %lu kb/s, up", name, (unsigned long)(smp->bps / 1000));
    else
        snprintf(buf, len, "%s, up", name);
}

/* One redraw per changed field, none while iconified (the text is picked up
 * when the window reopens). */
static void nsp_window_set_field(struct NspWindow *w, ULONG idx, const char *text)
{
    if (strcmp(w->text[idx], text) == 0)
        return;
    strlcpy(w->text[idx], text, NSP_TEXT_MAX);
    if (w->win != NULL)
        SetGadgetAttrs(w->field[idx], w->win, NULL, GA_Text, (ULONG)w->text[idx], TAG_DONE);
}

/* The six rates in field order, then the source line. */
void nsp_window_refresh(struct NspWindow *w, const struct NspSampler *smp,
                        const struct NspSettings *s)
{
    const ULONG rates[NSP_FIELD_IFACE] = {smp->curRx, smp->avgRx, smp->maxRx,
                                          smp->curTx, smp->avgTx, smp->maxTx};
    BOOL bits = NSP_VALUE(s, NSP_SET_UNITS) != 0;
    char buf[NSP_TEXT_MAX];
    for (ULONG i = 0; i < NSP_FIELD_IFACE; i++)
    {
        nsp_window_format_rate(buf, sizeof(buf), rates[i], bits, smp->valid);
        nsp_window_set_field(w, i, buf);
    }
    nsp_window_format_source(buf, sizeof(buf), smp, s);
    nsp_window_set_field(w, NSP_FIELD_IFACE, buf);
}

/* --- object construction -------------------------------------------------- */

/* Objects a failed parent left unadopted; NULL entries are skipped. */
static void nsp_window_dispose(Object **objs, ULONG n)
{
    for (ULONG i = 0; i < n; i++)
        if (objs[i] != NULL)
            DisposeObject(objs[i]);
}

/* n (<= NSP_ROW_MAX) labelled read-only text fields side by side, a titled
 * group box when title is given; field i shows w->text[first + i]. NULL
 * when any object failed, with everything made here disposed. Explicit
 * NewObject calls throughout: the reaction_macros "...End" closers hide
 * their ')' inside a macro, which cannot terminate this toolchain's vararg
 * NewObject macro. */
static Object *nsp_window_field_row(struct NspWindow *w, const char *title, ULONG first, ULONG n,
                                    const char *const *labels, const char *domain)
{
    Object *made[2 * NSP_ROW_MAX]; /* label, field, label, field, ... */
    struct TagItem tags[4 + 2 * NSP_ROW_MAX];
    ULONG t = 0;
    tags[t].ti_Tag = LAYOUT_Orientation;
    tags[t++].ti_Data = LAYOUT_ORIENT_HORIZ;
    if (title != NULL)
    {
        tags[t].ti_Tag = LAYOUT_BevelStyle;
        tags[t++].ti_Data = BVS_GROUP;
        tags[t].ti_Tag = LAYOUT_Label;
        tags[t++].ti_Data = (ULONG)title;
    }
    BOOL ok = TRUE;
    for (ULONG i = 0; i < n; i++)
    {
        strlcpy(w->text[first + i], "-", NSP_TEXT_MAX);
        made[2 * i] = NewObject(LABEL_GetClass(), NULL, LABEL_Text, (ULONG)labels[i], TAG_END);
        made[2 * i + 1] = NewObject(BUTTON_GetClass(), NULL,
            GA_ReadOnly, TRUE,
            GA_Text, (ULONG)w->text[first + i],
            BUTTON_DomainString, (ULONG)domain,
            TAG_END);
        ok = ok && made[2 * i] != NULL && made[2 * i + 1] != NULL;
        w->field[first + i] = (struct Gadget *)made[2 * i + 1];
        tags[t].ti_Tag = LAYOUT_AddChild;
        tags[t++].ti_Data = (ULONG)made[2 * i + 1];
        tags[t].ti_Tag = CHILD_Label;
        tags[t++].ti_Data = (ULONG)made[2 * i];
    }
    tags[t].ti_Tag = TAG_END;
    Object *row = ok ? NewObjectA(LAYOUT_GetClass(), NULL, tags) : NULL;
    if (row == NULL)
        nsp_window_dispose(made, 2 * n);
    return row;
}

/* The window contents: the two readout groups, the source line and the
 * graph, top to bottom; the graph takes the remaining height. */
static Object *nsp_window_root(struct NspWindow *w, struct Hook *renderHook)
{
    static const char *const rateLabels[3] = {"Current", "Average", "Maximum"};
    static const char *const sourceLabel[1] = {"Interface"};
    Object *part[4];
    part[0] = nsp_window_field_row(w, "Received", NSP_FIELD_RX_CUR, 3, rateLabels, "999.9 kB/s");
    part[1] = nsp_window_field_row(w, "Sent", NSP_FIELD_TX_CUR, 3, rateLabels, "999.9 kB/s");
    part[2] = nsp_window_field_row(w, NULL, NSP_FIELD_IFACE, 1, sourceLabel,
                                   "All interfaces, 1000 Mb/s, down");
    part[3] = NewObject(SPACE_GetClass(), NULL,
        GA_ReadOnly, TRUE,
        SPACE_Transparent, TRUE, /* the hook paints every pixel */
        SPACE_BevelStyle, BVS_FIELD,
        SPACE_MinWidth, 200,
        SPACE_MinHeight, 60,
        SPACE_RenderHook, (ULONG)renderHook,
        TAG_END);
    Object *root = NULL;
    if (part[0] != NULL && part[1] != NULL && part[2] != NULL && part[3] != NULL)
        root = NewObject(LAYOUT_GetClass(), NULL,
            LAYOUT_Orientation, LAYOUT_ORIENT_VERT,
            LAYOUT_SpaceOuter, TRUE,
            LAYOUT_DeferLayout, TRUE,
            LAYOUT_AddChild, (ULONG)part[0], CHILD_WeightedHeight, 0,
            LAYOUT_AddChild, (ULONG)part[1], CHILD_WeightedHeight, 0,
            LAYOUT_AddChild, (ULONG)part[2], CHILD_WeightedHeight, 0,
            LAYOUT_AddChild, (ULONG)part[3],
            TAG_END);
    if (root == NULL)
        nsp_window_dispose(part, 4);
    return root;
}

/* --- lifecycle ------------------------------------------------------------ */

/* Clamp a window box to the default public screen: a saved box may come from
 * a larger screen, and OpenWindow would refuse the window. */
static void nsp_window_fit_box(struct IBox *box)
{
    struct Screen *scr = LockPubScreen(NULL);
    if (scr == NULL)
        return;
    if (box->Width > scr->Width)
        box->Width = scr->Width;
    if (box->Height > scr->Height)
        box->Height = scr->Height;
    if (box->Left < 0)
        box->Left = 0;
    if (box->Top < 0)
        box->Top = 0;
    if (box->Left > scr->Width - box->Width)
        box->Left = (WORD)(scr->Width - box->Width);
    if (box->Top > scr->Height - box->Height)
        box->Top = (WORD)(scr->Height - box->Height);
    UnlockPubScreen(NULL, scr);
}

/* Everything up to the window object; on failure whatever was made is left
 * for nsp_window_exit(), which is always called. */
BOOL nsp_window_init(struct NspWindow *w, const struct NspSettings *s,
                     const struct NspIfaceSet *ifaces, struct Hook *renderHook)
{
    nsp_menu_sync(&w->menu, NULL, ifaces, s);

    WindowBase = OpenLibrary((CONST_STRPTR) "window.class", 47);
    LayoutBase = OpenLibrary((CONST_STRPTR) "gadgets/layout.gadget", 47);
    ButtonBase = OpenLibrary((CONST_STRPTR) "gadgets/button.gadget", 47);
    SpaceBase = OpenLibrary((CONST_STRPTR) "gadgets/space.gadget", 47);
    LabelBase = OpenLibrary((CONST_STRPTR) "images/label.image", 47);
    if (WindowBase == NULL || LayoutBase == NULL || ButtonBase == NULL || SpaceBase == NULL ||
        LabelBase == NULL)
    {
        nsp_report("Could not open the ReAction classes V47 (window.class, layout, button "
                   "and space gadgets, label image).");
        return FALSE;
    }
    w->appPort = CreateMsgPort();
    if (w->appPort == NULL)
    {
        nsp_report("Could not create a message port.");
        return FALSE;
    }

    Object *root = nsp_window_root(w, renderHook);
    if (root == NULL)
    {
        nsp_report("Could not create the window contents.");
        return FALSE;
    }

    struct IBox box = s->box;
    if (s->haveBox)
        nsp_window_fit_box(&box);

    /* the tool's own icon for the AppIcon; window.class disposes it */
    struct DiskObject *dobj =
        IconBase != NULL ? GetDiskObject((CONST_STRPTR) "PROGDIR:" NSP_NAME) : NULL;
    w->obj = NewObject(WINDOW_GetClass(), NULL,
        WA_Title, (ULONG)NSP_NAME,
        WA_DragBar, TRUE,
        WA_DepthGadget, TRUE,
        WA_SizeGadget, TRUE,
        WA_CloseGadget, TRUE,
        WA_Activate, TRUE,
        WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_MENUPICK | IDCMP_NEWSIZE | IDCMP_REFRESHWINDOW,
        s->haveBox ? WA_Left : TAG_IGNORE, (ULONG)box.Left,
        s->haveBox ? WA_Top : TAG_IGNORE, (ULONG)box.Top,
        WA_Width, (ULONG)(s->haveBox ? box.Width : NSP_DEFAULT_WIDTH),
        WA_Height, (ULONG)(s->haveBox ? box.Height : NSP_DEFAULT_HEIGHT),
        s->haveBox ? TAG_IGNORE : WINDOW_Position, WPOS_CENTERSCREEN,
        WINDOW_IconifyGadget, TRUE,
        WINDOW_AppPort, (ULONG)w->appPort,
        WINDOW_IconTitle, (ULONG)NSP_NAME,
        dobj != NULL ? WINDOW_Icon : TAG_IGNORE, (ULONG)dobj,
        WINDOW_NewMenu, (ULONG)w->menu.items,
        WINDOW_ParentGroup, (ULONG)root,
        TAG_END);
    if (w->obj == NULL)
    {
        DisposeObject(root);
        if (dobj != NULL)
            FreeDiskObject(dobj);
        nsp_report("Could not create the window.");
        return FALSE;
    }
    return TRUE;
}

/* Close, dispose (the window object owns the whole tree), then the classes. */
void nsp_window_exit(struct NspWindow *w)
{
    if (w->obj != NULL)
    {
        if (w->win != NULL)
            RA_CloseWindow(w->obj);
        w->win = NULL;
        DisposeObject(w->obj);
        w->obj = NULL;
    }
    if (w->appPort != NULL)
    {
        DeleteMsgPort(w->appPort);
        w->appPort = NULL;
    }
    struct Library **bases[] = {&LabelBase, &SpaceBase, &ButtonBase, &LayoutBase, &WindowBase};
    for (ULONG i = 0; i < sizeof(bases) / sizeof(bases[0]); i++)
        if (*bases[i] != NULL)
        {
            CloseLibrary(*bases[i]);
            *bases[i] = NULL;
        }
}

/* First open, or back from the AppIcon. */
BOOL nsp_window_open(struct NspWindow *w)
{
    w->win = RA_OpenWindow(w->obj);
    if (w->win == NULL)
    {
        nsp_report("Could not open the window.");
        return FALSE;
    }
    return TRUE;
}

/* To the AppIcon; window.class remembers the box for the reopen. */
void nsp_window_iconify(struct NspWindow *w)
{
    if (w->win != NULL && RA_Iconify(w->obj))
        w->win = NULL;
}

/* The window's UserPort while open, and the AppPort always: the AppIcon's
 * double-click arrives there, not through WINDOW_SigMask. */
ULONG nsp_window_sigmask(const struct NspWindow *w)
{
    ULONG mask = 0;
    if (w->win != NULL)
        GetAttr(WINDOW_SigMask, w->obj, &mask);
    return mask | (1UL << w->appPort->mp_SigBit);
}

/* The live window's outer box. */
BOOL nsp_window_get_box(const struct NspWindow *w, struct IBox *out)
{
    if (w->win == NULL)
        return FALSE;
    out->Left = w->win->LeftEdge;
    out->Top = w->win->TopEdge;
    out->Width = w->win->Width;
    out->Height = w->win->Height;
    return TRUE;
}

/* --- input ---------------------------------------------------------------- */

/* Keeps draining while iconified (WMHI_UNICONIFY arrives closed) and reopens
 * right there, so the rest of the drain sees an open window. */
ULONG nsp_window_handle_input(struct NspWindow *w, struct NspSettings *s)
{
    ULONG ev = 0;
    WORD code = 0;
    ULONG result;

    while ((result = RA_HandleInput(w->obj, &code)) != WMHI_LASTMSG)
    {
        switch (result & WMHI_CLASSMASK)
        {
        case WMHI_CLOSEWINDOW:
            return NSP_EV_QUIT;
        case WMHI_MENUPICK:
            if (w->win != NULL)
                ev |= nsp_menu_pick(&w->menu, w->win, s, (UWORD)(result & WMHI_MENUMASK));
            if (ev & NSP_EV_QUIT)
                return ev;
            break;
        case WMHI_ICONIFY:
            ev |= NSP_EV_ICONIFY;
            break;
        case WMHI_UNICONIFY:
            if (!nsp_window_open(w))
                return NSP_EV_QUIT;
            break;
        default:
            break;
        }
    }
    return ev;
}
