/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * NetSpeed menu: the strip window.class builds from a persistent NewMenu
 * array. The settings live here as checkmarked, mutually exclusive
 * sub-items; a pick writes straight into the NspSettings and comes back to
 * main() as an event mask.
 *
 *   Project   About... | Reset statistics | Iconify | Quit
 *   Settings  Interface » | <one sub-menu per nspSettings entry> » | Save
 *
 * Intuition moves the checkmarks on a pick; the array is rebuilt from the
 * settings after every change so a reopen (window.class recreates the strip
 * from the array) and a programmatic change (the selected interface
 * disappearing) show the right marks.
 */

#ifndef NSP_MENU_H
#define NSP_MENU_H

#include "netspeed.h"
#include "nsp_sample.h"

#include <intuition/classusr.h>
#include <intuition/intuition.h>
#include <libraries/gadtools.h>

/* Project (6), Settings title and Interface item, All + NSP_IFACE_MAX
 * interfaces, the table's items and sub-items, bar, Save, NM_END */
#define NSP_MENU_MAX 40

/* nsp_menu_pick() / nsp_window_handle_input() results */
#define NSP_EV_QUIT (1UL << 0)
#define NSP_EV_SOURCE (1UL << 1)  /* settings->interface changed */
#define NSP_EV_RESET (1UL << 2)   /* Project » Reset statistics */
#define NSP_EV_SAVE (1UL << 3)    /* Settings » Save */
#define NSP_EV_ICONIFY (1UL << 4) /* Project » Iconify, or the iconify gadget */

struct NspMenu
{
    struct NewMenu items[NSP_MENU_MAX]; /* persistent: window.class reads it at every open */
    ULONG count;
    char ifaceNames[NSP_IFACE_MAX][NSP_NAME_MAX]; /* sub-item label storage */
    ULONG ifaceCount;
};

/* Rebuild the array from the interface set and the settings, and hand it to
 * the window object (NULL before the window exists: build only). */
void nsp_menu_sync(struct NspMenu *m, Object *winObj, const struct NspIfaceSet *ifaces,
                   const struct NspSettings *s);
/* Walk a (multi-)selection: setting picks go into s, actions come back as
 * NSP_EV_* bits. The window must be open. */
ULONG nsp_menu_pick(struct NspMenu *m, struct Window *win, struct NspSettings *s, UWORD code);

#endif
