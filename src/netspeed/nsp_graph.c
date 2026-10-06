/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * NetSpeed graph — see nsp_graph.h.
 *
 * Two render paths share one column painter. The full repaint (render hook,
 * rescale, Reset, column time change) clears the area, draws the midline and
 * then the newest min(count, width) columns right to left. The incremental
 * path scrolls the area N pixels left (ScrollRaster fills the vacated
 * columns with the background pen) and paints the newest N columns.
 *
 * Drawing happens on the tool's task into the window's RastPort, under
 * AttemptLockLayerRom so a tick never blocks behind a window drag; the
 * render hook runs inside the gadget's GM_RENDER (refresh, resize, reopen),
 * where Intuition already holds the layer.
 */

#include "nsp_graph.h"

#include <exec/memory.h>
#include <gadgets/space.h>
#include <graphics/rastport.h>
#include <intuition/cghooks.h>
#include <intuition/classusr.h>
#include <intuition/gadgetclass.h>
#include <intuition/screens.h>

#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>

/* --- ring ----------------------------------------------------------------- */

/* Empty ring, empty accumulator, no automatic scale; the screen is stale. */
void nsp_graph_reset(struct NspGraph *g)
{
    g->head = 0;
    g->count = 0;
    g->accCount = 0;
    g->accRx = 0;
    g->accTx = 0;
    g->maxSeen = 0;
    g->undrawn = 0;
    g->needsRedraw = TRUE;
}

/* Samples per column from the seconds; unchanged is a no-op. */
void nsp_graph_set_column(struct NspGraph *g, LONG secs)
{
    ULONG samples = secs > 0 ? (ULONG)secs / NSP_TICK_SECS : 1;
    if (samples == 0)
        samples = 1;
    if (samples == g->samplesPerColumn)
        return;
    g->samplesPerColumn = samples;
    nsp_graph_reset(g);
}

/* Accumulate; every samplesPerColumn samples their mean becomes a column. */
void nsp_graph_push(struct NspGraph *g, ULONG rx, ULONG tx)
{
    g->accRx += rx;
    g->accTx += tx;
    if (++g->accCount < g->samplesPerColumn)
        return;
    rx = (ULONG)(g->accRx / g->accCount);
    tx = (ULONG)(g->accTx / g->accCount);
    g->accCount = 0;
    g->accRx = 0;
    g->accTx = 0;

    g->ring[g->head].rx = rx;
    g->ring[g->head].tx = tx;
    g->head = (g->head + 1) % g->capacity;
    if (g->count < g->capacity)
        g->count++;
    if (rx > g->maxSeen)
        g->maxSeen = rx;
    if (tx > g->maxSeen)
        g->maxSeen = tx;
    if (g->undrawn < g->capacity)
        g->undrawn++;
}

/* The link speed when asked for and known, else the automatic scale (never
 * below NSP_SCALE_MIN). A change makes the screen stale. */
void nsp_graph_set_scale(struct NspGraph *g, BOOL linkScale, ULONG linkBytesPerSec)
{
    ULONG wanted = g->maxSeen > NSP_SCALE_MIN ? g->maxSeen : NSP_SCALE_MIN;
    if (linkScale && linkBytesPerSec != 0)
        wanted = linkBytesPerSec;
    if (wanted != g->scale)
    {
        g->scale = wanted;
        g->needsRedraw = TRUE;
    }
}

/* --- rendering ------------------------------------------------------------ */

/* Rows and bytes-per-row from the area box and the scale. */
static void nsp_graph_geometry(struct NspGraph *g)
{
    const struct IBox *a = &g->area;
    g->mid = (WORD)(a->Top + a->Height / 2);
    g->halfUp = (WORD)(g->mid - a->Top);
    g->halfDown = (WORD)(a->Top + a->Height - 1 - g->mid);
    g->perPixel = g->halfUp > 0 ? g->scale / (ULONG)g->halfUp : g->scale;
    if (g->perPixel == 0)
        g->perPixel = 1;
}

/* One column: received up from the midline, sent down, each clipped to its
 * half (bursts above the link speed in link-scale mode). */
static void nsp_graph_column(const struct NspGraph *g, struct RastPort *rp, WORD x,
                             const struct NspColumn *col)
{
    ULONG up = col->rx / g->perPixel;
    ULONG down = col->tx / g->perPixel;
    if (up > (ULONG)g->halfUp)
        up = (ULONG)g->halfUp;
    if (down > (ULONG)g->halfDown)
        down = (ULONG)g->halfDown;
    if (up > 0)
    {
        SetAPen(rp, g->penRx);
        RectFill(rp, x, (WORD)(g->mid - (WORD)up), x, (WORD)(g->mid - 1));
    }
    if (down > 0)
    {
        SetAPen(rp, g->penTx);
        RectFill(rp, x, (WORD)(g->mid + 1), x, (WORD)(g->mid + (WORD)down));
    }
}

/* The newest n columns, newest at the right edge, older to the left. */
static void nsp_graph_columns(const struct NspGraph *g, struct RastPort *rp, WORD right, ULONG n)
{
    ULONG idx = g->head;
    for (ULONG i = 0; i < n; i++)
    {
        idx = (idx == 0 ? g->capacity : idx) - 1;
        nsp_graph_column(g, rp, (WORD)(right - (WORD)i), &g->ring[idx]);
    }
}

/* Everything from the ring: a narrow window shows the last Width columns. */
static void nsp_graph_redraw(struct NspGraph *g, struct RastPort *rp)
{
    const struct IBox *a = &g->area;
    g->needsRedraw = FALSE;
    g->undrawn = 0;
    if (a->Width <= 0 || a->Height <= 0)
        return;
    WORD right = (WORD)(a->Left + a->Width - 1);
    WORD bottom = (WORD)(a->Top + a->Height - 1);
    nsp_graph_geometry(g);

    SetDrMd(rp, JAM1);
    SetAPen(rp, g->penBg);
    RectFill(rp, a->Left, a->Top, right, bottom);
    SetAPen(rp, g->penMid);
    RectFill(rp, a->Left, g->mid, right, g->mid);
    nsp_graph_columns(g, rp, right, g->count < (ULONG)a->Width ? g->count : (ULONG)a->Width);
}

/* Scroll the graph n (< Width) columns left and paint the newest n. */
static void nsp_graph_scroll(struct NspGraph *g, struct RastPort *rp, ULONG n)
{
    const struct IBox *a = &g->area;
    g->undrawn = 0;
    WORD right = (WORD)(a->Left + a->Width - 1);
    WORD bottom = (WORD)(a->Top + a->Height - 1);

    SetDrMd(rp, JAM1);
    SetBPen(rp, g->penBg);
    ScrollRaster(rp, (WORD)n, 0, a->Left, a->Top, right, bottom);
    SetAPen(rp, g->penMid);
    RectFill(rp, (WORD)(right - (WORD)n + 1), g->mid, right, g->mid);
    nsp_graph_columns(g, rp, right, n);
}

/* Nothing to do, a full repaint, or a scroll by the undrawn columns; the
 * layer lock is only attempted so a drag or resize never stalls a tick. */
BOOL nsp_graph_draw(struct NspGraph *g, struct Window *win)
{
    if (win == NULL || !g->haveArea)
        return FALSE;
    if (!g->needsRedraw && g->undrawn == 0)
        return TRUE;
    if (!AttemptLockLayerRom(win->WLayer))
        return FALSE;
    if (g->needsRedraw || g->undrawn >= (ULONG)g->area.Width)
        nsp_graph_redraw(g, win->RPort);
    else
        nsp_graph_scroll(g, win->RPort, g->undrawn);
    UnlockLayerRom(win->WLayer);
    return TRUE;
}

/* space.gadget refresh: the box may have moved (resize, reopen) and the
 * screen pens may differ, so both are taken fresh every time. */
static VOID nsp_graph_render_hook(struct Hook *hook asm("a0"), Object *obj asm("a2"),
                                  struct gpRender *gpr asm("a1"))
{
    struct NspGraph *g = hook->h_Data;
    struct IBox *box = NULL;
    GetAttr(SPACE_AreaBox, obj, (ULONG *)&box);
    if (box == NULL || gpr->gpr_RPort == NULL)
        return;
    g->area = *box;

    /* the pen table: received bright, sent in the fill colour, midline dark */
    struct DrawInfo *dri = gpr->gpr_GInfo != NULL ? gpr->gpr_GInfo->gi_DrInfo : NULL;
    if (dri != NULL)
    {
        g->penBg = (UBYTE)dri->dri_Pens[BACKGROUNDPEN];
        g->penRx = (UBYTE)dri->dri_Pens[SHINEPEN];
        g->penTx = (UBYTE)dri->dri_Pens[FILLPEN];
        g->penMid = (UBYTE)dri->dri_Pens[SHADOWPEN];
    }
    g->haveArea = TRUE;
    nsp_graph_redraw(g, gpr->gpr_RPort);
}

/* --- lifecycle ------------------------------------------------------------ */

/* The ring (capacity columns) and the render hook; FALSE on no memory. */
BOOL nsp_graph_init(struct NspGraph *g, ULONG capacity)
{
    g->capacity = capacity > 0 ? capacity : 1;
    g->ring = AllocVec(g->capacity * sizeof(struct NspColumn), MEMF_ANY | MEMF_CLEAR);
    if (g->ring == NULL)
        return FALSE;
    g->scale = 0;
    g->samplesPerColumn = 1;
    g->haveArea = FALSE;
    g->penBg = 0;
    g->penRx = 1;
    g->penTx = 1;
    g->penMid = 1;
    nsp_graph_reset(g);

    g->hook.h_Entry = (APTR)nsp_graph_render_hook;
    g->hook.h_SubEntry = NULL;
    g->hook.h_Data = g;
    return TRUE;
}

/* Safe on a never-initialised graph. */
void nsp_graph_exit(struct NspGraph *g)
{
    if (g->ring != NULL)
        FreeVec(g->ring);
    g->ring = NULL;
}
