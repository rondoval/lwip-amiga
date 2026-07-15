/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * bsdsocket.library — resident structure and library lifecycle.
 *
 * Per-opener child bases: LibOpen copies the root base (jump table
 * included) and initializes the opener's private state — errno, fd table,
 * wait signal, WaitSelect timer. The opening task owns the copy; the
 * root only counts. The first open (under root->openLock) starts the
 * stack: netstack init, the timer task, and the netdev interface
 * bring-up with DHCP.
 */

#include "sb_base.h"

#include <exec/resident.h>

#include <minlist.h>

#include <debug.h>

#include "netstack.h"

LONG __attribute__((used, no_reorder)) doNotExecute(void);
LONG __attribute__((used, no_reorder)) doNotExecute(void)
{
    Kprintf("[bsdsocket] %s: entry stub executed\n", __func__);
    return -1;
}

extern const UBYTE endOfCode;
static const char libraryName[] = LIBRARY_NAME;
static const char libraryIdString[] = LIBRARY_IDSTRING;
static const APTR initTable[4];

const struct Resident bsdsocketResident __attribute__((used)) = {
    RTC_MATCHWORD,
    (struct Resident *)&bsdsocketResident,
    (APTR)&endOfCode,
    RTF_AUTOINIT,
    LIBRARY_VERSION,
    NT_LIBRARY,
    0,
    (APTR)&libraryName,
    (APTR)&libraryIdString,
    (APTR)initTable,
};

ULONG LibExpunge(struct SocketBase *base asm("a6"))
{
    Kprintf("[bsdsocket] %s: base 0x%08lx\n", __func__, (ULONG)base);
    struct SocketBase *root = SB_ROOT(base);

    if (root->libNode.lib_OpenCnt > 0)
    {
        root->libNode.lib_Flags |= LIBF_DELEXP;
        return 0;
    }

    /* orphaned ReleaseSocket parkings die with the library (the pool frees
     * their memory wholesale; the pcbs die with the stack) */
    sb_stack_stop(root);

    if (root->sockPool != NULL)
    {
        DeletePool(root->sockPool);
        root->sockPool = NULL;
    }
    _NewMinList(&root->releasedSockets);

    ULONG segList = root->segList;

    Forbid();
    Remove((struct Node *)root);
    Permit();

    ULONG size = (ULONG)root->libNode.lib_NegSize + root->libNode.lib_PosSize;
    FreeMem((APTR)((ULONG)root - root->libNode.lib_NegSize), size);

    return segList;
}

static struct Library *LibInit(struct Library *base asm("d0"), ULONG seglist asm("a0"), struct ExecBase *execBase asm("a6"))
{
    struct SocketBase *root = (struct SocketBase *)base;
    (void)execBase;

    root->segList = seglist;
    root->libNode.lib_Revision = (UWORD)LIBRARY_REVISION;
    root->root = NULL;
    root->stackTask = NULL;
    root->sockPool = NULL;
    root->openCount = 0;
    root->nextSockId = 1;
    root->defaultDomain[0] = '\0';
    _NewMinList(&root->releasedSockets);
    InitSemaphore(&root->openLock);

    Kprintf("[bsdsocket] initialized\n");
    return base;
}

static void child_cleanup(struct SocketBase *b)
{
    KprintfH("[bsdsocket] %s: base 0x%08lx\n", __func__, (ULONG)b);
    if (b->fd != NULL)
    {
        /* orphaned sockets die with the base */
        for (ULONG i = 0; i < SB_FD_COUNT; i++)
        {
            if (b->fd[i] != NULL)
            {
                netstack_lock();
                sb_sock_free(b, b->fd[i]);
                netstack_unlock();
                b->fd[i] = NULL;
            }
        }
        FreeMem(b->fd, SB_FD_COUNT * sizeof(struct SbSocket *));
        b->fd = NULL;
    }

    if (b->timerOpen)
    {
        CloseDevice(&b->timerReq->tr_node);
        b->timerOpen = FALSE;
    }
    if (b->timerReq != NULL)
    {
        DeleteIORequest(&b->timerReq->tr_node);
        b->timerReq = NULL;
    }
    if (b->timerPort != NULL)
    {
        DeleteMsgPort(b->timerPort);
        b->timerPort = NULL;
    }
    if (b->sigBit != -1)
    {
        FreeSignal(b->sigBit);
        b->sigBit = -1;
    }
}

struct SocketBase *LibOpen(ULONG version asm("d0"), struct SocketBase *base asm("a6"))
{
    /* exec 3.2 does the version check itself; D0 here is leftover junk */
    Kprintf("[bsdsocket] %s: task %s\n", __func__, (ULONG)FindTask(NULL)->tc_Node.ln_Name);
    struct SocketBase *root = SB_ROOT(base);
    (void)version;

    ObtainSemaphore(&root->openLock);

    if (root->stackTask == NULL)
    {
        if (root->sockPool == NULL)
            root->sockPool = CreatePool(MEMF_PUBLIC, 16384, 4096);
        if (root->sockPool == NULL || sb_stack_start(root) != 0)
        {
            Kprintf("[bsdsocket] stack startup failed\n");
            ReleaseSemaphore(&root->openLock);
            return NULL;
        }
    }

    /* the child base: a full copy including the jump table */
    ULONG negSize = root->libNode.lib_NegSize;
    ULONG posSize = root->libNode.lib_PosSize;
    UBYTE *mem = AllocMem(negSize + posSize, MEMF_PUBLIC);
    if (mem == NULL)
    {
        ReleaseSemaphore(&root->openLock);
        return NULL;
    }
    CopyMem((UBYTE *)root - negSize, mem, negSize + posSize);

    struct SocketBase *b = (struct SocketBase *)(mem + negSize);
    b->root = root;
    b->libNode.lib_OpenCnt = 1;
    b->libNode.lib_Flags &= (UBYTE)~LIBF_DELEXP;

    b->task = FindTask(NULL);
    b->sigBit = AllocSignal(-1);
    b->breakMask = SIGBREAKF_CTRL_C;
    b->sigIoMask = 0;
    b->sigUrgMask = 0;
    b->sigEventMask = 0;
    b->internalErrno = 0;
    b->errnoPtr = &b->internalErrno;
    b->errnoSize = sizeof(LONG);
    b->hErrno = 0;
    b->hErrnoPtr = &b->hErrno;
    b->logStat = 0;
    b->logTagPtr = NULL;
    b->logFacility = 0;
    b->logMask = SB_LOGMASK_ALL;
    b->timerPort = CreateMsgPort();
    b->timerReq = (struct timerequest *)CreateIORequest(b->timerPort, sizeof(struct timerequest));
    b->timerOpen = FALSE;
    b->dnsDone = FALSE;
    b->fd = AllocMem(SB_FD_COUNT * sizeof(struct SbSocket *), MEMF_PUBLIC | MEMF_CLEAR);

    if (b->timerReq != NULL &&
        OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_MICROHZ, &b->timerReq->tr_node, 0) == 0)
        b->timerOpen = TRUE;

    if (b->sigBit == -1 || b->fd == NULL || !b->timerOpen)
    {
        child_cleanup(b);
        FreeMem(mem, negSize + posSize);
        ReleaseSemaphore(&root->openLock);
        return NULL;
    }

    root->libNode.lib_OpenCnt++;
    root->libNode.lib_Flags &= (UBYTE)~LIBF_DELEXP;
    root->openCount++;

    ReleaseSemaphore(&root->openLock);
    return b;
}

ULONG LibClose(struct SocketBase *base asm("a6"))
{
    Kprintf("[bsdsocket] %s: base 0x%08lx, task %s\n", __func__, (ULONG)base, (ULONG)FindTask(NULL)->tc_Node.ln_Name);
    struct SocketBase *root = SB_ROOT(base);

    if (base->root == NULL)
    {
        /* closing the root directly — nothing sane to do */
        return 0;
    }

    child_cleanup(base);

    ULONG negSize = base->libNode.lib_NegSize;
    ULONG posSize = base->libNode.lib_PosSize;
    FreeMem((UBYTE *)base - negSize, negSize + posSize);

    ObtainSemaphore(&root->openLock);
    root->libNode.lib_OpenCnt--;
    root->openCount--;
    ReleaseSemaphore(&root->openLock);

    if (root->libNode.lib_OpenCnt == 0 && (root->libNode.lib_Flags & LIBF_DELEXP))
        return LibExpunge(root);

    return 0;
}

ULONG LibNull(void)
{
    Kprintf("[bsdsocket] %s called\n", __func__);
    return 0;
}

/* Placeholders for the LVOs not yet implemented: -1 for the LONG-returning
 * majority, NULL for the pointer-returning ones (an app checking != NULL
 * must not be handed -1). */
LONG LibStub(void)
{
    Kprintf("[bsdsocket] %s: unimplemented LVO called\n", __func__);
    return -1;
}

APTR LibStubNull(void)
{
    Kprintf("[bsdsocket] %s: unimplemented LVO called\n", __func__);
    return NULL;
}

static const APTR initTable[4] = {
    (APTR)sizeof(struct SocketBase),
    (APTR)bsdsocket_functable,
    NULL,
    (APTR)LibInit};
