/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * NetSpeed plot: a ring of samples (one per pixel column) rendered into the
 * window's space.gadget.
 *
 * Received bars grow up from the midline, sent bars down, both against one
 * scale: the highest rate seen since Reset (automatic) or the link speed.
 * The gadget's render hook (refresh, resize, uniconify) repaints everything
 * from the ring; a tick scrolls the plot one column and draws the new sample.
 */

#ifndef NSP_GRAPH_H
#define NSP_GRAPH_H

#include "netspeed.h"

#include <utility/hooks.h>

struct NspSample
{
    ULONG rx, tx; /* bytes/s */
};

struct NspGraph
{
    struct NspSample *ring;
    ULONG capacity; /* screen width at startup */
    ULONG head;     /* next write; newest = head - 1 */
    ULONG count;

    ULONG maxSeen; /* highest rate since Reset: the automatic scale */
    ULONG scale;   /* bytes/s at full half height */

    BOOL needsRedraw; /* on-screen plot is stale against the ring */
    BOOL pending;     /* newest sample not drawn yet */

    BOOL haveArea;   /* the hook has run: area and pens are valid */
    struct IBox area; /* SPACE_AreaBox, window coordinates */
    UBYTE penBg, penRx, penTx, penMid;

    struct Hook hook; /* SPACE_RenderHook; h_Data = this */
};

BOOL nsp_graph_init(struct NspGraph *g, ULONG capacity);
void nsp_graph_exit(struct NspGraph *g);
/* Forget the history and the automatic scale. */
void nsp_graph_reset(struct NspGraph *g);
void nsp_graph_push(struct NspGraph *g, ULONG rx, ULONG tx);
/* linkBytesPerSec = 0 when the link speed is unknown or the link is down. */
void nsp_graph_set_scale(struct NspGraph *g, BOOL linkScale, ULONG linkBytesPerSec);
/* Bring the plot up to date. FALSE when the window is closed or its layer is
 * busy (drag, resize): the samples stay in the ring, the next call catches up. */
BOOL nsp_graph_draw(struct NspGraph *g, struct Window *win);

#endif
