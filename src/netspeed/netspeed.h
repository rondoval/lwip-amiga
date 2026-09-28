/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * NetSpeed — definitions shared by all modules.
 *
 * Module map (each owns one sub-struct of NspContext, netspeed.c):
 *   nsp_prefs   the settings table, defaults, ENV:/ENVARC: file
 *   nsp_sample  interface set, byte counters, rates, link state
 *   nsp_graph   history ring and the graph inside the space.gadget
 *   nsp_menu    menu strip: the settings as checkmarked sub-items
 *   nsp_window  ReAction window, readouts, iconify, input
 */

#ifndef NETSPEED_H
#define NETSPEED_H

#include <stddef.h>

#include <exec/types.h>
#include <intuition/intuition.h>

#define NSP_NAME "NetSpeed"

#define NSP_NAME_MAX 32     /* interface name incl. NUL (Roadshow names are <= 15) */
#define NSP_IFACE_MAX 8     /* interfaces offered in the menu */
#define NSP_TEXT_MAX 48     /* one readout field ("<interface>, 1000 Mb/s, up") */
#define NSP_TICK_SECS 1     /* sample period: the stack refreshes its counters once a second */
#define NSP_AVG_MAX 300     /* longest moving-average window, in samples */
#define NSP_RESCAN_SECS 5   /* interface list poll period */
#define NSP_SCALE_MIN 1000  /* bytes/s: the automatic scale never goes below this */

/* One value a menu setting can take: what the menu shows, what the prefs
 * file says, and what it means to the module that consumes it. */
struct NspChoice
{
    const char *label;
    const char *token;
    LONG value;
};

/* A menu setting: a mutually exclusive group of choices. The table
 * (nspSettings, nsp_prefs.c) is the one place a setting is defined; the
 * defaults, the prefs file and the menu are all derived from it. */
struct NspSetting
{
    const char *label; /* Settings menu item */
    const char *key;   /* prefs file key */
    const struct NspChoice *choices;
    ULONG count;
    ULONG def; /* default choice */
};

enum
{
    NSP_SET_GRAPH,   /* seconds per graph column */
    NSP_SET_AVERAGE, /* average window in seconds, 0 = since Reset */
    NSP_SET_UNITS,   /* 0 bytes/s, 1 bits/s */
    NSP_SET_SCALE,   /* 0 automatic, 1 link speed */
    NSP_SET_COUNT
};

extern const struct NspSetting nspSettings[NSP_SET_COUNT];

/* The session's settings: loaded from ENV: at startup, edited live through
 * the menu, written by Settings » Save. Nothing else copies them. */
struct NspSettings
{
    char interface[NSP_NAME_MAX]; /* "" = all interfaces (stack-wide counters) */
    ULONG choice[NSP_SET_COUNT];  /* index into nspSettings[i].choices */
    BOOL haveBox;                 /* box is valid (from the prefs file) */
    struct IBox box;              /* outer window position and size */
};

/* the meaning of a setting's current choice */
#define NSP_VALUE(s, id) (nspSettings[id].choices[(s)->choice[id]].value)

extern BOOL nspFromWb;

/* Requester when started from Workbench, stderr line from the Shell. */
void nsp_report(const char *fmt, ...);
/* EasyRequest with the given "A|B|C" gadgets; returns the button number. */
LONG nsp_request(struct Window *parent, const char *gadgets, const char *fmt, ...);

#endif
