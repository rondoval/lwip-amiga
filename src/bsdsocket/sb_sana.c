/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * SANA-II backend, exec side: bring-up/teardown of an interface whose
 * driver speaks the classic SANA-II ABI, dispatched from sb_if_up/down.
 * The datapath glue (cooked-mode translation, the RX pump task, buffer
 * callbacks) lives in port/amiga/sana2_*.c.
 */

#include "sb_base.h"

#include <exec/io.h>

#include <debug.h>

#include <devices/sana2.h>
#include "netstack.h"
#include "sb_stack_priv.h"

LONG sb_sana_up(struct SbStackCtx *ctx, const struct NetCtlIfConfig *nif,
                LONG *aux)
{
    (void)nif;
    *aux = 0;
    Kprintf("[bsdsocket] SANA-II backend not built yet\n");
    sb_if_down(ctx);
    return NETCTL_ERR_DEVICE;
}

void sb_sana_down(struct SbStackCtx *ctx)
{
    (void)ctx; /* no backend state yet */
}
