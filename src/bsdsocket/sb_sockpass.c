/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * fd duplication and the inter-task socket handoff: Dup2Socket, and the
 * ObtainSocket / ReleaseSocket / ReleaseCopyOfSocket parking lot (released
 * sockets wait on the root's list under a caller-chosen or unique id until
 * another opener claims them).
 */

#include "sb_base.h"

#include <minlist.h>

#include <debug.h>

#include "netstack.h"

LONG bsd_Dup2Socket(LONG oldSock asm("d0"), LONG newSock asm("d1"),
                    struct SocketBase *base asm("a6"))
{
    KprintfT("[bsdsocket] %s: old=%ld new=%ld\n", __func__, oldSock, newSock);
    struct SbSocket *s = sb_fd_get(base, oldSock);
    if (s == NULL)
    {
        sb_set_errno(base, SB_EBADF);
        return -1;
    }

    netstack_lock();
    if (newSock == -1)
    {
        newSock = sb_fd_alloc(base, s);
        if (newSock < 0)
        {
            netstack_unlock();
            sb_set_errno(base, SB_EMFILE);
            return -1;
        }
    }
    else
    {
        if (newSock < 0 || newSock >= (LONG)base->fdCount)
        {
            netstack_unlock();
            sb_set_errno(base, SB_EBADF);
            return -1;
        }
        if (base->fd[newSock] == s)
        {
            netstack_unlock();
            return newSock;
        }
        if (base->fd[newSock] != NULL)
            sb_sock_free(base, base->fd[newSock]);
        base->fd[newSock] = s;
    }
    s->refs++;
    sb_owner_incref(s, base); /* same base, extra fd: bumps its existing slot */
    netstack_unlock();
    return newSock;
}

/* --------------------------------------- ObtainSocket / ReleaseSocket --- */

static struct SbReleased *sb_released_find(struct SocketBase *root, LONG id)
{
    KprintfT("[bsdsocket] %s: id=%ld\n", __func__, id);
    for (struct MinNode *n = root->releasedSockets.mlh_Head; n->mln_Succ != NULL; n = n->mln_Succ)
    {
        struct SbReleased *r = (struct SbReleased *)n;
        if (r->id == id)
            return r;
    }
    return NULL;
}

static LONG sb_release_common(struct SocketBase *base, LONG sock, LONG id, BOOL copy)
{
    KprintfT("[bsdsocket] %s: sock=%ld id=%ld\n", __func__, sock, id);
    struct SocketBase *root = SB_ROOT(base);
    struct SbSocket *s = sb_fd_get(base, sock);

    if (s == NULL)
    {
        sb_set_errno(base, SB_EBADF);
        return -1;
    }

    netstack_lock();
    if (id == -1 /* UNIQUE_ID */)
    {
        do
        {
            id = root->nextSockId++;
            if (root->nextSockId < 0)
                root->nextSockId = 1;
        } while (sb_released_find(root, id) != NULL);
    }
    else if (sb_released_find(root, id) != NULL)
    {
        netstack_unlock();
        sb_set_errno(base, SB_EADDRINUSE);
        return -1;
    }

    struct SbReleased *r = AllocPooled(root->sockPool, sizeof(struct SbReleased));
    if (r == NULL)
    {
        netstack_unlock();
        sb_set_errno(base, SB_ENOBUFS);
        return -1;
    }

    r->id = id;
    r->s = s;
    if (copy)
    {
        s->refs++; /* the releaser keeps its fd (and its owner slot) */
    }
    else
    {
        base->fd[sock] = NULL;
        /* the releaser hands its fd to the parking lot; drop its ownership but
         * keep refs (the park holds that reference). All owners gone ==
         * parked: nobody to wake until obtained. */
        sb_owner_decref(s, base);
    }
    AddTailMinList(&root->releasedSockets, &r->node);
    netstack_unlock();
    return id;
}

LONG bsd_ReleaseSocket(LONG sock asm("d0"), LONG id asm("d1"),
                       struct SocketBase *base asm("a6"))
{
    KprintfT("[bsdsocket] %s: sock=%ld id=%ld\n", __func__, sock, id);
    return sb_release_common(base, sock, id, FALSE);
}

LONG bsd_ReleaseCopyOfSocket(LONG sock asm("d0"), LONG id asm("d1"),
                             struct SocketBase *base asm("a6"))
{
    KprintfT("[bsdsocket] %s: sock=%ld id=%ld\n", __func__, sock, id);
    return sb_release_common(base, sock, id, TRUE);
}

LONG bsd_ObtainSocket(LONG id asm("d0"), LONG domain asm("d1"), LONG type asm("d2"),
                      LONG protocol asm("d3"), struct SocketBase *base asm("a6"))
{
    KprintfT("[bsdsocket] %s: id=%ld type=%ld\n", __func__, id, type);
    struct SocketBase *root = SB_ROOT(base);
    (void)domain;
    (void)protocol;

    netstack_lock();
    struct SbReleased *r = sb_released_find(root, id);
    if (r == NULL)
    {
        netstack_unlock();
        sb_set_errno(base, SB_EINVAL);
        return -1;
    }

    struct SbSocket *s = r->s;
    if (type != 0)
    {
        LONG have = s->type == SBT_TCP ? SB_SOCK_STREAM
                    : s->type == SBT_UDP ? SB_SOCK_DGRAM
                                         : SB_SOCK_RAW;
        if (have != type)
        {
            netstack_unlock();
            sb_set_errno(base, SB_EINVAL);
            return -1;
        }
    }

    LONG fd = sb_fd_alloc(base, s);
    if (fd < 0)
    {
        netstack_unlock();
        sb_set_errno(base, SB_EMFILE);
        return -1;
    }
    /* wakeups also target this opener; a copy keeps the releaser's slot, so
     * both are woken. The parked refcount transferred into this fd already,
     * so refs is not bumped here. */
    if (!sb_owner_incref(s, base))
    {
        base->fd[fd] = NULL;
        netstack_unlock();
        sb_set_errno(base, SB_ENOBUFS);
        return -1;
    }

    RemoveMinNode(&r->node);
    FreePooled(root->sockPool, r, sizeof(struct SbReleased));
    netstack_unlock();
    return fd;
}
