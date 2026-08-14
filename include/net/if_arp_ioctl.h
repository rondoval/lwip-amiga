/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * SIOC*ARP IoctlSocket() request codes for lwip-amiga's bsdsocket.library.
 *
 * The NDK's Roadshow netinclude defines struct arpreq and the ATF_* flags
 * (<net/if_arp.h>) but no ARP ioctl codes; this header supplies the classic
 * 4.3BSD/AmiTCP numbering so source written against those stacks compiles
 * unchanged. Include it after (or instead of) the NDK headers — everything
 * is guarded.
 *
 * Contract (all requests need any open socket, e.g. UDP):
 *   SIOCSARP  set an entry from arp_pa (sockaddr_in) + arp_ha (MAC in
 *             sa_data[0..5]). ATF_PERM in arp_flags makes it permanent;
 *             without it the entry ages out like a learned one. A non-
 *             permanent set over an existing permanent entry fails with
 *             EADDRINUSE (delete first). ATF_PUBL and ATF_USETRAILERS are
 *             not supported and rejected with EINVAL.
 *   SIOCGARP  look up arp_pa; fills arp_ha and arp_flags. ENXIO if absent.
 *   SIOCDARP  delete the entry for arp_pa, whatever its state. ENXIO if
 *             absent.
 *   SIOCGARPT dump the whole table, struct arptabreq below.
 *
 * Returned arp_flags: ATF_INUSE always; ATF_COM once the MAC is resolved
 * (absent = request still pending, arp_ha zeroed); ATF_PERM for permanent
 * entries. ATF_PUBL is never set.
 */

#ifndef NET_IF_ARP_IOCTL_H
#define NET_IF_ARP_IOCTL_H

#ifndef _NET_IF_ARP_H
#include <net/if_arp.h> /* struct arpreq, ATF_*; pulls sys/netinclude_types.h */
#endif
#ifndef _SYS_IOCCOM_H
#include <sys/ioccom.h> /* _IOW/_IOWR */
#endif

#ifndef SIOCSARP
#define SIOCSARP _IOW('i', 30, struct arpreq)  /* set entry     = 0x8024691E */
#define SIOCDARP _IOW('i', 32, struct arpreq)  /* delete entry  = 0x80246920 */
#define SIOCGARP _IOWR('i', 38, struct arpreq) /* get one entry = 0xC0246926 */
#endif

#ifndef SIOCGARPT
/* Whole-table dump. The caller sets atr_size to the capacity of atr_table
 * (in entries) — or 0/NULL for a pure size query — and one call fills up to
 * that many entries atomically, setting atr_size to the number copied and
 * atr_inuse to the number currently present in the table. */
struct arptabreq
{
	__LONG atr_size;           /* in: capacity; out: entries copied */
	__LONG atr_inuse;          /* out: entries present in the table */
	struct arpreq *atr_table;  /* caller array, may be NULL */
};
#define SIOCGARPT _IOWR('i', 91, struct arptabreq) /* = 0xC00C695B */
#endif

#endif /* NET_IF_ARP_IOCTL_H */
