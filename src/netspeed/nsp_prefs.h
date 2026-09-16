/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * NetSpeed settings: defaults and value clamps. The ENV:/ENVARC: file comes in
 * a later stage.
 */

#ifndef NSP_PREFS_H
#define NSP_PREFS_H

#include "netspeed.h"

void nsp_prefs_defaults(struct NspSettings *s);

/* The one interval clamp, used by the file reader and the integer gadget. */
LONG nsp_prefs_clamp_interval(LONG v);

#endif
