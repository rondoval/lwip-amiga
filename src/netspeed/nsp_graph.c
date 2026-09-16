/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * NetSpeed plot — see nsp_graph.h.
 *
 * Two render paths share one column painter. The full repaint (render hook,
 * rescale, Reset, catch-up) clears the area, draws the midline and then the
 * newest min(count, width) samples right to left. The per-tick path scrolls
 * the area one pixel left (ScrollRaster fills the vacated column with the
 * background pen) and paints the newest sample into the last column.
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

void nsp_graph_reset(struct NspGraph *g)
{
    g->head = 0;
    g->count = 0;
    g->maxSeen = 0;
    g->pending = FALSE;
    g->needsRedraw = TRUE;
}

void nsp_graph_push(struct NspGraph *g, ULONG rx, ULONG tx)
{
    g->ring[g->head].rx = rx;
    g->ring[g->head].tx = tx;
    g->head = (g->head + 1) % g->capacity;
    if (g->count < g->capacity)
        g->count++;
    if (rx > g->maxSeen)
        g->maxSeen = rx;
    if (tx > g->maxSeen)
        g->maxSeen = tx;
    /* the scroll path draws exactly one column: a second undrawn sample
     * means a repaint */
    if (g->pending)
        g->needsRedraw = TRUE;
    g->pending = TRUE;
}

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

/* rows and bytes-per-row from the area box and the scale */
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

/* one column: received up from the midline, sent down, each clipped to
 * its half (bursts above the link speed in link-scale mode) */
static void nsp_graph_column(const struct NspGraph *g, struct RastPort *rp, WORD x,
                             const struct NspSample *smp)
{
    ULONG up = smp->rx / g->perPixel;
    ULONG down = smp->tx / g->perPixel;
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

static void nsp_graph_redraw(struct NspGraph *g, struct RastPort *rp)
{
    const struct IBox *a = &g->area;
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

    /* newest at the right edge; a narrow window shows the last Width samples */
    ULONG visible = g->count < (ULONG)a->Width ? g->count : (ULONG)a->Width;
    ULONG idx = g->head;
    for (ULONG i = 0; i < visible; i++)
    {
        idx = (idx == 0 ? g->capacity : idx) - 1;
        nsp_graph_column(g, rp, (WORD)(right - (WORD)i), &g->ring[idx]);
    }
    g->needsRedraw = FALSE;
    g->pending = FALSE;
}

static void nsp_graph_scroll(struct NspGraph *g, struct RastPort *rp)
{
    const struct IBox *a = &g->area;
    if (a->Width <= 0 || a->Height <= 0)
        return;
    WORD right = (WORD)(a->Left + a->Width - 1);
    WORD bottom = (WORD)(a->Top + a->Height - 1);

    SetDrMd(rp, JAM1);
    SetBPen(rp, g->penBg);
    ScrollRaster(rp, 1, 0, a->Left, a->Top, right, bottom);
    SetAPen(rp, g->penMid);
    WritePixel(rp, right, g->mid);
    ULONG newest = (g->head == 0 ? g->capacity : g->head) - 1;
    nsp_graph_column(g, rp, right, &g->ring[newest]);
    g->pending = FALSE;
}

BOOL nsp_graph_draw(struct NspGraph *g, struct Window *win)
{
    if (win == NULL || !g->haveArea)
        return FALSE;
    if (!g->needsRedraw && !g->pending)
        return TRUE;
    /* the window is being dragged or sized: keep sampling, paint later */
    if (!AttemptLockLayerRom(win->WLayer))
    {
        g->needsRedraw = TRUE;
        return FALSE;
    }
    if (g->needsRedraw)
        nsp_graph_redraw(g, win->RPort);
    else
        nsp_graph_scroll(g, win->RPort);
    UnlockLayerRom(win->WLayer);
    return TRUE;
}

/* space.gadget refresh: the box may have moved (resize, reopen) and the
 * screen pens may differ, so both are taken fresh every time */
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

BOOL nsp_graph_init(struct NspGraph *g, ULONG capacity)
{
    g->capacity = capacity > 0 ? capacity : 1;
    g->ring = AllocVec(g->capacity * sizeof(struct NspSample), MEMF_ANY | MEMF_CLEAR);
    if (g->ring == NULL)
        return FALSE;
    g->scale = 0;
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

void nsp_graph_exit(struct NspGraph *g)
{
    if (g->ring != NULL)
        FreeVec(g->ring);
    g->ring = NULL;
}
