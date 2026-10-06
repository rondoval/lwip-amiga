/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * NetSpeed settings: the settings table, the defaults and the preferences
 * file.
 *
 * ENV:NetSpeed.prefs is read at startup; Settings » Save writes it and
 * ENVARC:NetSpeed.prefs. Flat "KEY = VALUE" lines like netstack.prefs:
 *
 *   INTERFACE = genet          (absent: all interfaces)
 *   GRAPH = 1 | 5 | 30         (seconds per graph column)
 *   AVERAGE = 10 | 60 | 300 | 0  (average window in seconds, 0 = since Reset)
 *   UNITS = BYTES | BITS
 *   SCALE = AUTO | LINK
 *   WINDOW = left top width height   (absent: centred, default size)
 *
 * The four enumerated keys and their words are the nspSettings table's keys
 * and tokens. Keys are case-insensitive, unknown keys are ignored, a value
 * outside its table keeps the default. A missing file is not an error.
 */

#ifndef NSP_PREFS_H
#define NSP_PREFS_H

#include "netspeed.h"

/* All interfaces, every setting at its table default, no saved box. */
void nsp_prefs_defaults(struct NspSettings *s);

/* Defaults, then whatever ENV:NetSpeed.prefs overrides. Never reports. */
void nsp_prefs_load(struct NspSettings *s);

/* ENV: then ENVARC:; both are attempted. FALSE with the first failing path
 * and its IoErr() code. */
BOOL nsp_prefs_save(const struct NspSettings *s, const char **failedPath, LONG *ioErr);

#endif
