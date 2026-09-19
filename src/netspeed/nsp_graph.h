/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * NetSpeed graph: a ring of columns (one per pixel) rendered into the
 * window's space.gadget.
 *
 * Received bars grow up from the midline, sent bars down, both against one
 * scale: the highest column since Reset (automatic) or the link speed. A
 * column is the mean of samplesPerColumn samples. The gadget's render hook
 * (refresh, resize, uniconify) repaints everything from the ring; a draw
 * after N new columns scrolls the graph by N and paints just those.
 */

#ifndef NSP_GRAPH_H
#define NSP_GRAPH_H

#include "netspeed.h"

#include <utility/hooks.h>

struct NspColumn
{
    ULONG rx, tx; /* bytes/s */
};

struct NspGraph
{
    struct NspColumn *ring;
    ULONG capacity; /* screen width at startup */
    ULONG head;     /* next write; newest = head - 1 */
    ULONG count;

    /* a column is the mean of samplesPerColumn samples, gathered here */
    ULONG samplesPerColumn;
    ULONG accCount;
    unsigned long long accRx, accTx;

    ULONG maxSeen; /* highest column since Reset: the automatic scale */
    ULONG scale;   /* bytes/s at full half height */

    BOOL needsRedraw; /* on-screen graph is stale beyond its newest columns */
    ULONG undrawn;    /* columns pushed since the last draw, at most capacity */

    BOOL haveArea;    /* the hook has run: area, pens and geometry are valid */
    struct IBox area; /* SPACE_AreaBox, window coordinates */
    WORD mid;         /* midline row */
    WORD halfUp;      /* rows above the midline (received) */
    WORD halfDown;    /* rows below the midline (sent) */
    ULONG perPixel;   /* bytes/s per row, from scale and halfUp */
    UBYTE penBg, penRx, penTx, penMid;

    struct Hook hook; /* SPACE_RenderHook; h_Data = this */
};

BOOL nsp_graph_init(struct NspGraph *g, ULONG capacity);
void nsp_graph_exit(struct NspGraph *g);
/* Forget the history and the automatic scale. */
void nsp_graph_reset(struct NspGraph *g);
/* Seconds per column; a change clears the graph (its time base is gone). */
void nsp_graph_set_column(struct NspGraph *g, LONG secs);
/* One sample (bytes/s); every samplesPerColumn of them make a column. */
void nsp_graph_push(struct NspGraph *g, ULONG rx, ULONG tx);
/* linkBytesPerSec = 0 when the link speed is unknown or the link is down. */
void nsp_graph_set_scale(struct NspGraph *g, BOOL linkScale, ULONG linkBytesPerSec);
/* Bring the graph up to date. FALSE when the window is closed or its layer
 * is busy (drag, resize): the columns stay in the ring, the next call
 * catches up. */
BOOL nsp_graph_draw(struct NspGraph *g, struct Window *win);

#endif
