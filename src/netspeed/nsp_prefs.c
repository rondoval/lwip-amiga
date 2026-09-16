/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * NetSpeed settings — see nsp_prefs.h.
 */

#include "nsp_prefs.h"

void nsp_prefs_defaults(struct NspSettings *s)
{
    s->interface[0] = '\0';
    s->interval = NSP_INTERVAL_MIN;
    s->bits = FALSE;
    s->linkScale = FALSE;
    s->haveBox = FALSE;
    s->box.Left = 0;
    s->box.Top = 0;
    s->box.Width = 0;
    s->box.Height = 0;
}

LONG nsp_prefs_clamp_interval(LONG v)
{
    if (v < NSP_INTERVAL_MIN)
        return NSP_INTERVAL_MIN;
    if (v > NSP_INTERVAL_MAX)
        return NSP_INTERVAL_MAX;
    return v;
}
