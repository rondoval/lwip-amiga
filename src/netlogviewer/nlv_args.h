/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * NetLogViewer arguments
 */

#ifndef NLV_ARGS_H
#define NLV_ARGS_H

#include "nlv.h"

#define NLV_DEFAULT_POPKEY "shift alt f8"

struct NlvArgs
{
    char popKey[NLV_KEY_MAX]; /* CX_POPKEY: commodities key description */
    LONG priority;            /* CX_PRIORITY: broker priority */
    BOOL popup;               /* CX_POPUP: open the window at start */
};

/* argc == 0 means a Workbench start with argv = the WBStartup message.
 * FALSE = bad arguments, already reported. */
BOOL nlv_args_parse(int argc, char **argv, struct NlvArgs *a);

#endif /* NLV_ARGS_H */
