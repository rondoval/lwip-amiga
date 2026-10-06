/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * NetSpeed window: the ReAction objects, the readout fields, iconification
 * and the input drain. The menu (nsp_menu) lives inside it.
 *
 * Layout:
 *   Received group   Current [ ] Average [ ] Maximum [ ]
 *   Sent group       Current [ ] Average [ ] Maximum [ ]
 *   Interface [ genet, 1000 Mb/s, up ]
 *   graph (space.gadget, takes the remaining height)
 *
 * The window never iconifies on its own: it reports NSP_EV_ICONIFY and
 * main() calls nsp_window_iconify() once everything else (Save) has seen the
 * open window.
 */

#ifndef NSP_WINDOW_H
#define NSP_WINDOW_H

#include "netspeed.h"
#include "nsp_menu.h"
#include "nsp_sample.h"

#include <exec/ports.h>
#include <intuition/classusr.h>
#include <utility/hooks.h>

/* the six rate fields first, in the order nsp_window_refresh() fills them */
enum
{
    NSP_FIELD_RX_CUR,
    NSP_FIELD_RX_AVG,
    NSP_FIELD_RX_MAX,
    NSP_FIELD_TX_CUR,
    NSP_FIELD_TX_AVG,
    NSP_FIELD_TX_MAX,
    NSP_FIELD_IFACE, /* "<source>, <link>" */
    NSP_FIELD_COUNT
};

struct NspWindow
{
    Object *obj;
    struct Window *win;      /* NULL while iconified */
    struct MsgPort *appPort; /* AppIcon messages while iconified */

    struct Gadget *field[NSP_FIELD_COUNT];
    char text[NSP_FIELD_COUNT][NSP_TEXT_MAX]; /* button.gadget keeps the pointer */

    struct NspMenu menu;
};

/* Classes, ports, objects and the menu array; the window stays closed. */
BOOL nsp_window_init(struct NspWindow *w, const struct NspSettings *s,
                     const struct NspIfaceSet *ifaces, struct Hook *renderHook);
/* Safe on a never-initialised (zeroed) window. */
void nsp_window_exit(struct NspWindow *w);
BOOL nsp_window_open(struct NspWindow *w);
void nsp_window_iconify(struct NspWindow *w);

/* The bits to Wait() on; the window's own bit is absent while iconified. */
ULONG nsp_window_sigmask(const struct NspWindow *w);
/* Drain the window's messages; returns NSP_EV_* bits. */
ULONG nsp_window_handle_input(struct NspWindow *w, struct NspSettings *s);
/* Readouts from the sampler, in the units the settings ask for. */
void nsp_window_refresh(struct NspWindow *w, const struct NspSampler *smp,
                        const struct NspSettings *s);
/* Outer window box; FALSE while iconified. */
BOOL nsp_window_get_box(const struct NspWindow *w, struct IBox *out);

#endif
