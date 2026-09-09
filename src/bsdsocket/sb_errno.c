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

/* The BSD strerror texts for the codes this library sets (sb_base.h SB_E*) */
struct SbErrText
{
    UBYTE code;
    const char *text;
};

static const struct SbErrText sbErrnoText[] = {
    {SB_EINTR, "Interrupted system call"},
    {SB_ENXIO, "Device not configured"},
    {SB_EBADF, "Bad file descriptor"},
    {SB_ENOMEM, "Cannot allocate memory"},
    {SB_EACCES, "Permission denied"},
    {SB_EFAULT, "Bad address"},
    {SB_EINVAL, "Invalid argument"},
    {SB_EMFILE, "Too many open files"},
    {SB_EPIPE, "Broken pipe"},
    {SB_EWOULDBLOCK, "Resource temporarily unavailable"},
    {SB_EINPROGRESS, "Operation now in progress"},
    {SB_EALREADY, "Operation already in progress"},
    {SB_ENOTSOCK, "Socket operation on non-socket"},
    {SB_EDESTADDRREQ, "Destination address required"},
    {SB_EMSGSIZE, "Message too long"},
    {SB_ENOPROTOOPT, "Protocol not available"},
    {SB_EPROTONOSUPPORT, "Protocol not supported"},
    {SB_ESOCKTNOSUPPORT, "Socket type not supported"},
    {SB_EOPNOTSUPP, "Operation not supported"},
    {SB_EAFNOSUPPORT, "Address family not supported by protocol family"},
    {SB_EADDRINUSE, "Address already in use"},
    {SB_EADDRNOTAVAIL, "Can't assign requested address"},
    {SB_ENETUNREACH, "Network is unreachable"},
    {SB_ECONNABORTED, "Software caused connection abort"},
    {SB_ECONNRESET, "Connection reset by peer"},
    {SB_ENOBUFS, "No buffer space available"},
    {SB_EISCONN, "Socket is already connected"},
    {SB_ENOTCONN, "Socket is not connected"},
    {SB_ESHUTDOWN, "Can't send after socket shutdown"},
    {SB_ETIMEDOUT, "Operation timed out"},
    {SB_ECONNREFUSED, "Connection refused"},
    {SB_EHOSTUNREACH, "No route to host"},
};

static const struct SbErrText sbHerrnoText[] = {
    {SB_HOST_NOT_FOUND, "Unknown host"},
    {SB_TRY_AGAIN, "Host name lookup failure"},
    {SB_NO_RECOVERY, "Unknown server error"},
    {SB_NO_DATA, "No address associated with name"},
};

static const char *sb_err_lookup(const struct SbErrText *tab, ULONG n, LONG code,
                                 const char *unknown)
{
    if (code == 0)
        return "No error";
    for (ULONG i = 0; i < n; i++)
        if (tab[i].code == code)
            return tab[i].text;
    return unknown;
}

const char *sb_errno_text(LONG code)
{
    return sb_err_lookup(sbErrnoText, sizeof(sbErrnoText) / sizeof(sbErrnoText[0]), code,
                         "Unknown error");
}

const char *sb_herrno_text(LONG code)
{
    return sb_err_lookup(sbHerrnoText, sizeof(sbHerrnoText) / sizeof(sbHerrnoText[0]), code,
                         "Unknown resolver error");
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
