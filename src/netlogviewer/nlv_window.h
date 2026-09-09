/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * NetLogViewer window: the ReAction message list, its menu and the save
 * requester. The list lives on whether the window is open or not — Hide
 * closes the window, the lines stay.
 */

#ifndef NLV_WINDOW_H
#define NLV_WINDOW_H

#include "nlv.h"

/* class libraries + objects; FALSE = reported to the user */
BOOL nlv_window_init(void);
void nlv_window_exit(void);

BOOL nlv_window_show(void); /* open, or bring to front when already open */
void nlv_window_hide(void);
ULONG nlv_window_sigmask(void); /* 0 while hidden */

/* Batch ingest: begin detaches the list from the gadget, add appends one
 * line (evicting the oldest past NLV_MAX_LINES), end re-attaches and scrolls
 * to the newest line. One begin/end per drained burst keeps redraws to one. */
void nlv_window_update_begin(void);
void nlv_window_update_add(const struct NlvLogMsg *m);
void nlv_window_update_end(void);

enum NlvAction
{
    NLV_ACT_NONE,
    NLV_ACT_HIDE,
    NLV_ACT_QUIT
};

/* Drain the window's input; Clear and Save are handled inside. */
enum NlvAction nlv_window_handle_input(void);

#endif /* NLV_WINDOW_H */
