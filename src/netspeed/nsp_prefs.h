/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * NetSpeed settings: defaults, value clamps and the preferences file.
 *
 * ENV:NetSpeed.prefs is read at startup; Settings » Save writes it and
 * ENVARC:NetSpeed.prefs. Flat "KEY = VALUE" lines like netstack.prefs:
 *
 *   INTERFACE = genet          (absent: all interfaces)
 *   INTERVAL = 1               (seconds, 1..30)
 *   UNITS = BYTES | BITS
 *   SCALE = AUTO | LINK
 *   WINDOW = left top width height   (absent: centred, default size)
 *
 * Keys are case-insensitive, unknown keys are ignored, a bad value keeps
 * the default. A missing file is not an error.
 */

#ifndef NSP_PREFS_H
#define NSP_PREFS_H

#include "netspeed.h"

void nsp_prefs_defaults(struct NspSettings *s);

/* The one interval clamp, used by the file reader and the integer gadget. */
LONG nsp_prefs_clamp_interval(LONG v);

/* Defaults, then whatever ENV:NetSpeed.prefs overrides. Never reports. */
void nsp_prefs_load(struct NspSettings *s);

/* ENV: then ENVARC:; both are attempted. FALSE with the first failing path
 * and its IoErr() code. */
BOOL nsp_prefs_save(const struct NspSettings *s, const char **failedPath, LONG *ioErr);

#endif
