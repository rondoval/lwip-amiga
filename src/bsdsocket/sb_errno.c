/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * errno / h_errno plumbing: the per-opener error cells, their optional
 * mirrors into caller-registered storage (SetErrnoPtr and the SBTC_ERRNO*
 * tags), and the Errno/SetErrnoPtr LVOs.
 */

#include "sb_base.h"

#include <debug.h>

void sb_set_errno(struct SocketBase *base, LONG code)
{
    KprintfT("[bsdsocket] %s: code=%ld\n", __func__, code);
    base->internalErrno = code;
    if (base->errnoPtr != NULL && base->errnoPtr != &base->internalErrno)
    {
        switch (base->errnoSize)
        {
        case 1:
            *(BYTE *)base->errnoPtr = (BYTE)code;
            break;
        case 2:
            *(WORD *)base->errnoPtr = (WORD)code;
            break;
        default:
            *(LONG *)base->errnoPtr = code;
            break;
        }
    }
}

void sb_set_herrno(struct SocketBase *base, LONG code)
{
    KprintfT("[bsdsocket] %s: code=%ld\n", __func__, code);
    base->hErrno = code;
    if (base->hErrnoPtr != NULL)
        *base->hErrnoPtr = code;
}

/* the API-wide fail idiom: set errno, return -1 */
LONG sb_fail(struct SocketBase *base, LONG code)
{
    KprintfT("[bsdsocket] %s: code %ld\n", __func__, code);
    sb_set_errno(base, code);
    return -1;
}

LONG bsd_Errno(struct SocketBase *base asm("a6"))
{
    KprintfT("[bsdsocket] %s: errno=%ld\n", __func__, base->internalErrno);
    return base->internalErrno;
}

VOID bsd_SetErrnoPtr(APTR errnoPtr asm("a0"), LONG size asm("d0"),
                     struct SocketBase *base asm("a6"))
{
    KprintfT("[bsdsocket] %s: ptr=0x%08lx size=%ld\n", __func__, (ULONG)errnoPtr, size);
    if (errnoPtr != NULL && (size == 1 || size == 2 || size == 4))
    {
        base->errnoPtr = errnoPtr;
        base->errnoSize = (ULONG)size;
    }
    else
    {
        base->errnoPtr = &base->internalErrno;
        base->errnoSize = sizeof(LONG);
    }
}
