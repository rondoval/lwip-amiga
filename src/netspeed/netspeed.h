/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * NetSpeed — definitions shared by all modules.
 *
 * Module map (each owns one sub-struct of NspContext, netspeed.c):
 *   nsp_prefs   settings defaults, clamps, ENV:/ENVARC: file
 *   nsp_sample  interface set, byte counters, rates, link state
 *   nsp_graph   history ring and the plot inside the space.gadget
 *   nsp_window  ReAction window, menus, readouts, iconify
 */

#ifndef NETSPEED_H
#define NETSPEED_H

#include <stddef.h>

#include <exec/types.h>
#include <intuition/intuition.h>

#define NSP_NAME "NetSpeed"

#define NSP_NAME_MAX 32     /* interface name incl. NUL (Roadshow names are <= 15) */
#define NSP_IFACE_MAX 8     /* interfaces offered by the chooser */
#define NSP_TEXT_MAX 24     /* one readout field */
#define NSP_INTERVAL_MIN 1  /* the stack refreshes its counters once a second */
#define NSP_INTERVAL_MAX 30
#define NSP_RESCAN_SECS 5   /* interface list poll period */
#define NSP_SCALE_MIN 1000  /* bytes/s: the automatic scale never goes below this */

/* The session's settings: loaded from ENV: at startup, edited live through the
 * window, written by Settings » Save. Nothing else copies them. */
struct NspSettings
{
    char interface[NSP_NAME_MAX]; /* "" = all interfaces (stack-wide counters) */
    LONG interval;                /* seconds between samples, 1..30 */
    BOOL bits;                    /* readouts in bits/s instead of bytes/s */
    BOOL linkScale;               /* plot full scale = link speed, else automatic */
    BOOL haveBox;                 /* box is valid (from the prefs file) */
    struct IBox box;              /* outer window position and size */
};

extern BOOL nspFromWb;

/* libnix.a has strlcpy, but under -mcrt=nix20 its <string.h> only declares it
 * when __NO_INLINE__ is set. */
__stdargs size_t strlcpy(char *dst, const char *src, size_t size);

/* Requester when started from Workbench, stderr line from the Shell. */
void nsp_report(const char *fmt, ...);
/* EasyRequest with the given "A|B|C" gadgets; returns the button number. */
LONG nsp_request(struct Window *parent, const char *gadgets, const char *fmt, ...);

#endif
