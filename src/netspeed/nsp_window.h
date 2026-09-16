/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * NetSpeed window: the ReAction objects, the menus, the readout fields and
 * iconification. User edits go straight into the NspSettings (clamped) and
 * come back to main() as an event mask.
 */

#ifndef NSP_WINDOW_H
#define NSP_WINDOW_H

#include "netspeed.h"
#include "nsp_sample.h"

#include <exec/lists.h>
#include <exec/ports.h>
#include <intuition/classusr.h>
#include <utility/hooks.h>

enum
{
    NSP_FIELD_RX_CUR,
    NSP_FIELD_RX_AVG,
    NSP_FIELD_RX_MAX,
    NSP_FIELD_TX_CUR,
    NSP_FIELD_TX_AVG,
    NSP_FIELD_TX_MAX,
    NSP_FIELD_LINK,
    NSP_FIELD_COUNT
};

/* nsp_window_handle_input() results */
#define NSP_EV_QUIT (1UL << 0)
#define NSP_EV_SOURCE (1UL << 1)   /* settings->interface changed */
#define NSP_EV_INTERVAL (1UL << 2) /* settings->interval changed */
#define NSP_EV_UNITS (1UL << 3)    /* settings->bits changed */
#define NSP_EV_SCALE (1UL << 4)    /* settings->linkScale changed */
#define NSP_EV_RESET (1UL << 5)    /* Project » Reset statistics */
#define NSP_EV_SAVE (1UL << 6)     /* Settings » Save */
#define NSP_EV_REOPENED (1UL << 7) /* back from the icon: readouts need a refresh */

struct NspWindow
{
    Object *obj;
    struct Window *win;      /* NULL while iconified */
    struct MsgPort *appPort; /* AppIcon messages while iconified */

    struct Gadget *iface, *interval, *units, *scale, *space;
    struct Gadget *field[NSP_FIELD_COUNT];
    char text[NSP_FIELD_COUNT][NSP_TEXT_MAX]; /* button.gadget keeps the pointer */

    struct List ifaceLabels;                      /* chooser nodes */
    char ifaceNames[NSP_IFACE_MAX][NSP_NAME_MAX]; /* node text storage */
    ULONG ifaceCount;

    struct IBox lastBox; /* captured at iconify */
    BOOL haveLastBox;
};

BOOL nsp_window_init(struct NspWindow *w, const struct NspSettings *s,
                     const struct NspIfaceSet *ifaces, struct Hook *renderHook);
void nsp_window_exit(struct NspWindow *w);
BOOL nsp_window_open(struct NspWindow *w);
void nsp_window_iconify(struct NspWindow *w);

ULONG nsp_window_sigmask(const struct NspWindow *w); /* 0 while iconified */
ULONG nsp_window_appsig(const struct NspWindow *w);
ULONG nsp_window_handle_input(struct NspWindow *w, struct NspSettings *s);

/* Rebuild the chooser from the set and select the named entry ("" = All). */
void nsp_window_set_ifaces(struct NspWindow *w, const struct NspIfaceSet *ifaces,
                           const char *selected);
void nsp_window_select_iface(struct NspWindow *w, const char *name);
void nsp_window_refresh(struct NspWindow *w, const struct NspSampler *smp,
                        const struct NspSettings *s);
/* Outer window box: live, or the one captured at iconify. */
BOOL nsp_window_get_box(const struct NspWindow *w, struct IBox *out);

#endif
