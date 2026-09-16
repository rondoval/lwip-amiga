/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * NetSpeed plot — see nsp_graph.h.
 *
 * Stage 1: ring and scale bookkeeping; the render hook paints the background
 * only. The bars arrive with the next stage.
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

static void nsp_graph_redraw(struct NspGraph *g, struct RastPort *rp)
{
    const struct IBox *a = &g->area;
    if (a->Width <= 0 || a->Height <= 0)
        return;
    SetDrMd(rp, JAM1);
    SetAPen(rp, g->penBg);
    RectFill(rp, a->Left, a->Top, a->Left + a->Width - 1, a->Top + a->Height - 1);
    g->needsRedraw = FALSE;
    g->pending = FALSE;
}

BOOL nsp_graph_draw(struct NspGraph *g, struct Window *win)
{
    if (win == NULL || !g->haveArea)
        return FALSE;
    /* the window is being dragged or sized: keep sampling, paint later */
    if (!AttemptLockLayerRom(win->WLayer))
    {
        g->needsRedraw = TRUE;
        return FALSE;
    }
    if (g->needsRedraw)
        nsp_graph_redraw(g, win->RPort);
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

    struct DrawInfo *dri = gpr->gpr_GInfo != NULL ? gpr->gpr_GInfo->gi_DrInfo : NULL;
    if (dri != NULL)
    {
        g->penBg = (UBYTE)dri->dri_Pens[BACKGROUNDPEN];
        g->penRx = (UBYTE)dri->dri_Pens[FILLPEN];
        g->penTx = (UBYTE)dri->dri_Pens[SHADOWPEN];
        g->penMid = (UBYTE)dri->dri_Pens[SHINEPEN];
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
