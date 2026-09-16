/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * NetSpeed window — see nsp_window.h.
 *
 * Layout:
 *   Settings group   Interface [chooser]   Interval (s) [integer]
 *                    Units     [chooser]   Scale        [chooser]
 *   Received group   Current [ ] Average [ ] Maximum [ ]
 *   Sent group       Current [ ] Average [ ] Maximum [ ]
 *   Link [ ]
 *   plot (space.gadget, takes the remaining height)
 */

#include "nsp_window.h"
#include "nsp_prefs.h"

#include <stdio.h>
#include <string.h>

#include <classes/window.h>
#include <gadgets/button.h>
#include <gadgets/chooser.h>
#include <gadgets/integer.h>
#include <gadgets/layout.h>
#include <gadgets/space.h>
#include <images/bevel.h>
#include <images/label.h>
#include <intuition/gadgetclass.h>
#include <intuition/intuition.h>
#include <libraries/bsdsocket.h>
#include <libraries/gadtools.h>
#include <reaction/reaction_macros.h>
#include <workbench/workbench.h>

#include <clib/alib_protos.h>
#include <proto/button.h>
#include <proto/chooser.h>
#include <proto/exec.h>
#include <proto/icon.h>
#include <proto/integer.h>
#include <proto/intuition.h>
#include <proto/label.h>
#include <proto/layout.h>
#include <proto/space.h>
#include <proto/window.h>

struct Library *WindowBase;
struct Library *LayoutBase;
struct Library *ChooserBase;
struct Library *IntegerBase;
struct Library *ButtonBase;
struct Library *SpaceBase;
struct Library *LabelBase;

#define NSP_DEFAULT_WIDTH 520
#define NSP_DEFAULT_HEIGHT 300

enum
{
    GID_IFACE = 1,
    GID_INTERVAL,
    GID_UNITS,
    GID_SCALE,
    GID_GRAPH
};

enum
{
    MID_ABOUT = 1,
    MID_RESET,
    MID_ICONIFY,
    MID_QUIT,
    MID_SAVE
};

static struct NewMenu nspMenu[] = {
    {NM_TITLE, (STRPTR) "Project", NULL, 0, 0, NULL},
    {NM_ITEM, (STRPTR) "About...", (STRPTR) "?", 0, 0, (APTR)MID_ABOUT},
    {NM_ITEM, (STRPTR) "Reset statistics", (STRPTR) "R", 0, 0, (APTR)MID_RESET},
    {NM_ITEM, (STRPTR) "Iconify", (STRPTR) "I", 0, 0, (APTR)MID_ICONIFY},
    {NM_ITEM, NM_BARLABEL, NULL, 0, 0, NULL},
    {NM_ITEM, (STRPTR) "Quit", (STRPTR) "Q", 0, 0, (APTR)MID_QUIT},
    {NM_TITLE, (STRPTR) "Settings", NULL, 0, 0, NULL},
    {NM_ITEM, (STRPTR) "Save", (STRPTR) "S", 0, 0, (APTR)MID_SAVE},
    {NM_END, NULL, NULL, 0, 0, NULL},
};

static STRPTR nspUnitsLabels[] = {(STRPTR) "bytes/s", (STRPTR) "bits/s", NULL};
static STRPTR nspScaleLabels[] = {(STRPTR) "Automatic", (STRPTR) "Link speed", NULL};

/* --- interface chooser list ----------------------------------------------- */

static void nsp_ifaces_free(struct NspWindow *w)
{
    struct Node *n;
    while ((n = RemHead(&w->ifaceLabels)) != NULL)
        FreeChooserNode(n);
    w->ifaceCount = 0;
}

/* "All interfaces" first, then the set; the nodes point into ifaceNames */
static void nsp_ifaces_fill(struct NspWindow *w, const struct NspIfaceSet *set)
{
    struct Node *n = AllocChooserNode(CNA_Text, (ULONG) "All interfaces", TAG_DONE);
    if (n == NULL)
        return;
    AddTail(&w->ifaceLabels, n);
    for (ULONG i = 0; i < set->count && i < NSP_IFACE_MAX; i++)
    {
        strlcpy(w->ifaceNames[i], set->names[i], NSP_NAME_MAX);
        n = AllocChooserNode(CNA_Text, (ULONG)w->ifaceNames[i], TAG_DONE);
        if (n == NULL)
            return;
        AddTail(&w->ifaceLabels, n);
        w->ifaceCount++;
    }
}

/* chooser index of a name: 0 = All interfaces, also for an unknown name */
static LONG nsp_iface_index(const struct NspWindow *w, const char *name)
{
    if (name[0] == '\0')
        return 0;
    for (ULONG i = 0; i < w->ifaceCount; i++)
        if (strcmp(w->ifaceNames[i], name) == 0)
            return (LONG)i + 1;
    return 0;
}

void nsp_window_set_ifaces(struct NspWindow *w, const struct NspIfaceSet *ifaces,
                           const char *selected)
{
    /* detach before touching the list, reattach with the selection */
    SetGadgetAttrs(w->iface, w->win, NULL, CHOOSER_Labels, ~0UL, TAG_DONE);
    nsp_ifaces_free(w);
    nsp_ifaces_fill(w, ifaces);
    SetGadgetAttrs(w->iface, w->win, NULL, CHOOSER_Labels, (ULONG)&w->ifaceLabels,
                   CHOOSER_Selected, nsp_iface_index(w, selected), TAG_DONE);
}

void nsp_window_select_iface(struct NspWindow *w, const char *name)
{
    SetGadgetAttrs(w->iface, w->win, NULL, CHOOSER_Selected, nsp_iface_index(w, name), TAG_DONE);
}

/* --- readouts ------------------------------------------------------------- */

/* "12.3 MB/s" / "98.6 Mb/s": one decimal, decimal prefixes (1000) so the
 * numbers line up with a "1000 Mb/s" link */
static void nsp_format_rate(char *buf, size_t len, ULONG bytesPerSec, BOOL bits, BOOL valid)
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
                     (unsigned long)(v % scale[i].div / (scale[i].div / 10)), scale[i].prefix, unit);
            return;
        }
    snprintf(buf, len, "%lu %c/s", (unsigned long)v, unit);
}

static void nsp_format_link(char *buf, size_t len, const struct NspSampler *smp)
{
    if (!smp->haveLink)
        strlcpy(buf, "n/a", len);
    else if (smp->state != SM_Up)
        strlcpy(buf, "down", len);
    else if (smp->bps >= 1000000)
        snprintf(buf, len, "%lu Mb/s, up", (unsigned long)(smp->bps / 1000000));
    else if (smp->bps > 0)
        snprintf(buf, len, "%lu kb/s, up", (unsigned long)(smp->bps / 1000));
    else
        strlcpy(buf, "up", len);
}

/* one redraw per changed field, none while iconified (the text is picked up
 * when the window reopens) */
static void nsp_set_field(struct NspWindow *w, ULONG idx, const char *text)
{
    if (strcmp(w->text[idx], text) == 0)
        return;
    strlcpy(w->text[idx], text, NSP_TEXT_MAX);
    if (w->win != NULL)
        SetGadgetAttrs(w->field[idx], w->win, NULL, GA_Text, (ULONG)w->text[idx], TAG_DONE);
}

void nsp_window_refresh(struct NspWindow *w, const struct NspSampler *smp,
                        const struct NspSettings *s)
{
    static const ULONG order[6] = {NSP_FIELD_RX_CUR, NSP_FIELD_RX_AVG, NSP_FIELD_RX_MAX,
                                   NSP_FIELD_TX_CUR, NSP_FIELD_TX_AVG, NSP_FIELD_TX_MAX};
    const ULONG rates[6] = {smp->curRx, smp->avgRx, smp->maxRx,
                            smp->curTx, smp->avgTx, smp->maxTx};
    char buf[NSP_TEXT_MAX];
    for (ULONG i = 0; i < 6; i++)
    {
        nsp_format_rate(buf, sizeof(buf), rates[i], s->bits, smp->valid);
        nsp_set_field(w, order[i], buf);
    }
    nsp_format_link(buf, sizeof(buf), smp);
    nsp_set_field(w, NSP_FIELD_LINK, buf);
}

/* --- object construction -------------------------------------------------- */

/* Objects made but not yet adopted by a parent, so a failure half-way can
 * dispose exactly what exists. A parent takes over everything made since
 * its mark. */
#define NSP_BUILD_MAX 32
struct NspBuild
{
    Object *own[NSP_BUILD_MAX];
    ULONG n;
    BOOL failed;
};

static Object *nsp_own(struct NspBuild *b, Object *o)
{
    if (o == NULL)
        b->failed = TRUE;
    else if (b->n < NSP_BUILD_MAX)
        b->own[b->n++] = o;
    return o;
}

static Object *nsp_parent(struct NspBuild *b, ULONG mark, Object *parent)
{
    if (parent == NULL)
    {
        b->failed = TRUE;
        return NULL;
    }
    b->n = mark;
    b->own[b->n++] = parent;
    return parent;
}

static void nsp_build_fail(struct NspBuild *b)
{
    while (b->n > 0)
        DisposeObject(b->own[--b->n]);
}

static Object *nsp_label(struct NspBuild *b, const char *text)
{
    return nsp_own(b, NewObject(LABEL_GetClass(), NULL, LABEL_Text, (ULONG)text, TAG_END));
}

static Object *nsp_field(struct NspBuild *b, struct NspWindow *w, ULONG idx, const char *domain)
{
    strlcpy(w->text[idx], "-", NSP_TEXT_MAX);
    Object *o = nsp_own(b, NewObject(BUTTON_GetClass(), NULL,
        GA_ReadOnly, TRUE,
        GA_Text, (ULONG)w->text[idx],
        BUTTON_DomainString, (ULONG)domain,
        TAG_END));
    w->field[idx] = (struct Gadget *)o;
    return o;
}

#define NSP_ROW_MAX 3

/* A layout holding up to NSP_ROW_MAX labelled children; with a title it is a
 * group box. Takes over the children (and their labels) made since mark. */
static Object *nsp_layout(struct NspBuild *b, ULONG mark, ULONG orient, const char *title,
                          ULONG n, Object *const *children, Object *const *labels)
{
    if (b->failed)
        return NULL;
    struct TagItem tags[4 + 2 * NSP_ROW_MAX];
    ULONG t = 0;
    tags[t].ti_Tag = LAYOUT_Orientation;
    tags[t++].ti_Data = orient;
    if (title != NULL)
    {
        tags[t].ti_Tag = LAYOUT_BevelStyle;
        tags[t++].ti_Data = BVS_GROUP;
        tags[t].ti_Tag = LAYOUT_Label;
        tags[t++].ti_Data = (ULONG)title;
    }
    for (ULONG i = 0; i < n && i < NSP_ROW_MAX; i++)
    {
        tags[t].ti_Tag = LAYOUT_AddChild;
        tags[t++].ti_Data = (ULONG)children[i];
        if (labels != NULL && labels[i] != NULL)
        {
            tags[t].ti_Tag = CHILD_Label;
            tags[t++].ti_Data = (ULONG)labels[i];
        }
    }
    tags[t].ti_Tag = TAG_END;
    return nsp_parent(b, mark, NewObjectA(LAYOUT_GetClass(), NULL, tags));
}

/* Current | Average | Maximum in a titled group box */
static Object *nsp_readout_group(struct NspBuild *b, struct NspWindow *w, const char *title,
                                 ULONG first)
{
    static const char *const names[3] = {"Current", "Average", "Maximum"};
    ULONG mark = b->n;
    Object *children[3];
    Object *labels[3];
    for (ULONG i = 0; i < 3; i++)
    {
        labels[i] = nsp_label(b, names[i]);
        children[i] = nsp_field(b, w, first + i, "999.9 kB/s");
    }
    return nsp_layout(b, mark, LAYOUT_ORIENT_HORIZ, title, 3, children, labels);
}

static Object *nsp_settings_group(struct NspBuild *b, struct NspWindow *w,
                                  const struct NspSettings *s)
{
    ULONG mark = b->n;

    ULONG m1 = b->n;
    Object *lIface = nsp_label(b, "Interface");
    w->iface = (struct Gadget *)nsp_own(b, NewObject(CHOOSER_GetClass(), NULL,
        GA_ID, GID_IFACE,
        GA_RelVerify, TRUE,
        CHOOSER_PopUp, TRUE,
        CHOOSER_Labels, (ULONG)&w->ifaceLabels,
        CHOOSER_Selected, nsp_iface_index(w, s->interface),
        TAG_END));
    Object *lInterval = nsp_label(b, "Interval (s)");
    w->interval = (struct Gadget *)nsp_own(b, NewObject(INTEGER_GetClass(), NULL,
        GA_ID, GID_INTERVAL,
        GA_RelVerify, TRUE,
        INTEGER_Number, s->interval,
        INTEGER_Minimum, NSP_INTERVAL_MIN,
        INTEGER_Maximum, NSP_INTERVAL_MAX,
        INTEGER_MaxChars, 2,
        INTEGER_Arrows, TRUE,
        TAG_END));
    Object *c1[2] = {(Object *)w->iface, (Object *)w->interval};
    Object *l1[2] = {lIface, lInterval};
    Object *row1 = nsp_layout(b, m1, LAYOUT_ORIENT_HORIZ, NULL, 2, c1, l1);

    ULONG m2 = b->n;
    Object *lUnits = nsp_label(b, "Units");
    w->units = (struct Gadget *)nsp_own(b, NewObject(CHOOSER_GetClass(), NULL,
        GA_ID, GID_UNITS,
        GA_RelVerify, TRUE,
        CHOOSER_PopUp, TRUE,
        CHOOSER_LabelArray, (ULONG)nspUnitsLabels,
        CHOOSER_Selected, s->bits ? 1 : 0,
        TAG_END));
    Object *lScale = nsp_label(b, "Scale");
    w->scale = (struct Gadget *)nsp_own(b, NewObject(CHOOSER_GetClass(), NULL,
        GA_ID, GID_SCALE,
        GA_RelVerify, TRUE,
        CHOOSER_PopUp, TRUE,
        CHOOSER_LabelArray, (ULONG)nspScaleLabels,
        CHOOSER_Selected, s->linkScale ? 1 : 0,
        TAG_END));
    Object *c2[2] = {(Object *)w->units, (Object *)w->scale};
    Object *l2[2] = {lUnits, lScale};
    Object *row2 = nsp_layout(b, m2, LAYOUT_ORIENT_HORIZ, NULL, 2, c2, l2);

    Object *rows[2] = {row1, row2};
    return nsp_layout(b, mark, LAYOUT_ORIENT_VERT, "Settings", 2, rows, NULL);
}

static Object *nsp_link_row(struct NspBuild *b, struct NspWindow *w)
{
    ULONG mark = b->n;
    Object *label = nsp_label(b, "Link");
    Object *field = nsp_field(b, w, NSP_FIELD_LINK, "1000 Mb/s, down");
    return nsp_layout(b, mark, LAYOUT_ORIENT_HORIZ, NULL, 1, &field, &label);
}

static Object *nsp_root(struct NspBuild *b, struct NspWindow *w, const struct NspSettings *s,
                        struct Hook *renderHook)
{
    ULONG mark = b->n;
    Object *settings = nsp_settings_group(b, w, s);
    Object *received = nsp_readout_group(b, w, "Received", NSP_FIELD_RX_CUR);
    Object *sent = nsp_readout_group(b, w, "Sent", NSP_FIELD_TX_CUR);
    Object *link = nsp_link_row(b, w);
    w->space = (struct Gadget *)nsp_own(b, NewObject(SPACE_GetClass(), NULL,
        GA_ID, GID_GRAPH,
        GA_ReadOnly, TRUE,
        SPACE_Transparent, TRUE, /* the hook paints every pixel */
        SPACE_BevelStyle, BVS_FIELD,
        SPACE_MinWidth, 200,
        SPACE_MinHeight, 60,
        SPACE_RenderHook, (ULONG)renderHook,
        TAG_END));
    if (b->failed)
        return NULL;
    return nsp_parent(b, mark, NewObject(LAYOUT_GetClass(), NULL,
        LAYOUT_Orientation, LAYOUT_ORIENT_VERT,
        LAYOUT_SpaceOuter, TRUE,
        LAYOUT_DeferLayout, TRUE,
        LAYOUT_AddChild, (ULONG)settings, CHILD_WeightedHeight, 0,
        LAYOUT_AddChild, (ULONG)received, CHILD_WeightedHeight, 0,
        LAYOUT_AddChild, (ULONG)sent, CHILD_WeightedHeight, 0,
        LAYOUT_AddChild, (ULONG)link, CHILD_WeightedHeight, 0,
        LAYOUT_AddChild, (ULONG)w->space,
        TAG_END));
}

/* --- lifecycle ------------------------------------------------------------ */

/* clamp a window box to the default public screen */
static void nsp_fit_box(struct IBox *box)
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

BOOL nsp_window_init(struct NspWindow *w, const struct NspSettings *s,
                     const struct NspIfaceSet *ifaces, struct Hook *renderHook)
{
    NewList(&w->ifaceLabels);
    w->ifaceCount = 0;
    w->haveLastBox = FALSE;

    WindowBase = OpenLibrary((CONST_STRPTR) "window.class", 47);
    LayoutBase = OpenLibrary((CONST_STRPTR) "gadgets/layout.gadget", 47);
    ChooserBase = OpenLibrary((CONST_STRPTR) "gadgets/chooser.gadget", 47);
    IntegerBase = OpenLibrary((CONST_STRPTR) "gadgets/integer.gadget", 47);
    ButtonBase = OpenLibrary((CONST_STRPTR) "gadgets/button.gadget", 47);
    SpaceBase = OpenLibrary((CONST_STRPTR) "gadgets/space.gadget", 47);
    LabelBase = OpenLibrary((CONST_STRPTR) "images/label.image", 47);
    if (WindowBase == NULL || LayoutBase == NULL || ChooserBase == NULL || IntegerBase == NULL ||
        ButtonBase == NULL || SpaceBase == NULL || LabelBase == NULL)
    {
        nsp_report("Could not open the ReAction classes V47 (window.class, layout, chooser, "
                   "integer, button and space gadgets, label image).");
        return FALSE;
    }
    w->appPort = CreateMsgPort();
    if (w->appPort == NULL)
    {
        nsp_report("Could not create a message port.");
        return FALSE;
    }
    nsp_ifaces_fill(w, ifaces);

    /* explicit NewObject calls: the reaction_macros "...End" closers hide
     * their ')' inside a macro, which cannot terminate this toolchain's
     * vararg NewObject macro */
    struct NspBuild b;
    b.n = 0;
    b.failed = FALSE;
    Object *root = nsp_root(&b, w, s, renderHook);
    if (root == NULL)
    {
        nsp_build_fail(&b);
        nsp_report("Could not create the window contents.");
        return FALSE;
    }

    /* a saved box may come from a larger screen: fit it to this one, or
     * OpenWindow would refuse the window */
    struct IBox box = s->box;
    if (s->haveBox)
        nsp_fit_box(&box);

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
        WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_MENUPICK | IDCMP_NEWSIZE | IDCMP_REFRESHWINDOW |
                      IDCMP_GADGETUP,
        s->haveBox ? WA_Left : TAG_IGNORE, (ULONG)box.Left,
        s->haveBox ? WA_Top : TAG_IGNORE, (ULONG)box.Top,
        WA_Width, (ULONG)(s->haveBox ? box.Width : NSP_DEFAULT_WIDTH),
        WA_Height, (ULONG)(s->haveBox ? box.Height : NSP_DEFAULT_HEIGHT),
        s->haveBox ? TAG_IGNORE : WINDOW_Position, WPOS_CENTERSCREEN,
        WINDOW_IconifyGadget, TRUE,
        WINDOW_AppPort, (ULONG)w->appPort,
        WINDOW_IconTitle, (ULONG)NSP_NAME,
        dobj != NULL ? WINDOW_Icon : TAG_IGNORE, (ULONG)dobj,
        WINDOW_NewMenu, (ULONG)nspMenu,
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

void nsp_window_exit(struct NspWindow *w)
{
    if (w->obj != NULL)
    {
        if (w->win != NULL)
            RA_CloseWindow(w->obj);
        w->win = NULL;
        SetGadgetAttrs(w->iface, NULL, NULL, CHOOSER_Labels, ~0UL, TAG_DONE);
        DisposeObject(w->obj);
        w->obj = NULL;
    }
    nsp_ifaces_free(w);
    if (w->appPort != NULL)
    {
        DeleteMsgPort(w->appPort);
        w->appPort = NULL;
    }
    struct Library **bases[] = {&LabelBase, &SpaceBase, &ButtonBase, &IntegerBase,
                                &ChooserBase, &LayoutBase, &WindowBase};
    for (ULONG i = 0; i < sizeof(bases) / sizeof(bases[0]); i++)
        if (*bases[i] != NULL)
        {
            CloseLibrary(*bases[i]);
            *bases[i] = NULL;
        }
}

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

void nsp_window_iconify(struct NspWindow *w)
{
    if (w->win == NULL)
        return;
    w->lastBox.Left = w->win->LeftEdge;
    w->lastBox.Top = w->win->TopEdge;
    w->lastBox.Width = w->win->Width;
    w->lastBox.Height = w->win->Height;
    w->haveLastBox = TRUE;
    if (RA_Iconify(w->obj))
        w->win = NULL;
}

ULONG nsp_window_sigmask(const struct NspWindow *w)
{
    if (w->win == NULL)
        return 0;
    ULONG mask = 0;
    GetAttr(WINDOW_SigMask, w->obj, &mask);
    return mask;
}

ULONG nsp_window_appsig(const struct NspWindow *w)
{
    return 1UL << w->appPort->mp_SigBit;
}

BOOL nsp_window_get_box(const struct NspWindow *w, struct IBox *out)
{
    if (w->win != NULL)
    {
        out->Left = w->win->LeftEdge;
        out->Top = w->win->TopEdge;
        out->Width = w->win->Width;
        out->Height = w->win->Height;
        return TRUE;
    }
    if (!w->haveLastBox)
        return FALSE;
    *out = w->lastBox;
    return TRUE;
}

/* --- input ---------------------------------------------------------------- */

static void nsp_about(struct NspWindow *w)
{
    nsp_request(w->win, "OK",
                NSP_NAME " " TOOL_VERSION " " TOOL_DATE "\n\n"
                "Network throughput per interface.\n"
                "Received above the line, sent below,\n"
                "both halves on one scale.");
}

static ULONG nsp_gadget_up(struct NspWindow *w, struct NspSettings *s, UWORD id)
{
    ULONG v = 0;
    switch (id)
    {
    case GID_IFACE:
        GetAttr(CHOOSER_Selected, (Object *)w->iface, &v);
        strlcpy(s->interface, v >= 1 && v <= w->ifaceCount ? w->ifaceNames[v - 1] : "",
                NSP_NAME_MAX);
        return NSP_EV_SOURCE;
    case GID_INTERVAL:
        GetAttr(INTEGER_Number, (Object *)w->interval, &v);
        s->interval = nsp_prefs_clamp_interval((LONG)v);
        if ((LONG)v != s->interval)
            SetGadgetAttrs(w->interval, w->win, NULL, INTEGER_Number, s->interval, TAG_DONE);
        return NSP_EV_INTERVAL;
    case GID_UNITS:
        GetAttr(CHOOSER_Selected, (Object *)w->units, &v);
        s->bits = v != 0;
        return NSP_EV_UNITS;
    case GID_SCALE:
        GetAttr(CHOOSER_Selected, (Object *)w->scale, &v);
        s->linkScale = v != 0;
        return NSP_EV_SCALE;
    default:
        return 0;
    }
}

static ULONG nsp_menu_pick(struct NspWindow *w, UWORD code)
{
    ULONG ev = 0;
    BOOL iconify = FALSE;

    while (code != MENUNULL && w->win != NULL)
    {
        struct MenuItem *item = ItemAddress(w->win->MenuStrip, code);
        if (item == NULL)
            break;
        switch ((ULONG)GTMENUITEM_USERDATA(item))
        {
        case MID_ABOUT:
            nsp_about(w);
            break;
        case MID_RESET:
            ev |= NSP_EV_RESET;
            break;
        case MID_ICONIFY:
            /* latched: the rest of a multi-selection still runs */
            iconify = TRUE;
            break;
        case MID_QUIT:
            return NSP_EV_QUIT;
        case MID_SAVE:
            ev |= NSP_EV_SAVE;
            break;
        default:
            break;
        }
        code = item->NextSelect;
    }
    if (iconify)
        nsp_window_iconify(w);
    return ev;
}

ULONG nsp_window_handle_input(struct NspWindow *w, struct NspSettings *s)
{
    ULONG ev = 0;
    WORD code = 0;
    ULONG result;

    /* keeps draining while iconified: WMHI_UNICONIFY arrives closed */
    while ((result = RA_HandleInput(w->obj, &code)) != WMHI_LASTMSG)
    {
        switch (result & WMHI_CLASSMASK)
        {
        case WMHI_CLOSEWINDOW:
            return NSP_EV_QUIT;
        case WMHI_MENUPICK:
            ev |= nsp_menu_pick(w, (UWORD)(result & WMHI_MENUMASK));
            if (ev & NSP_EV_QUIT)
                return ev;
            break;
        case WMHI_GADGETUP:
            ev |= nsp_gadget_up(w, s, (UWORD)(result & WMHI_GADGETMASK));
            break;
        case WMHI_ICONIFY:
            nsp_window_iconify(w);
            break;
        case WMHI_UNICONIFY:
            if (!nsp_window_open(w))
                return NSP_EV_QUIT;
            ev |= NSP_EV_REOPENED;
            break;
        default:
            break;
        }
    }
    return ev;
}
