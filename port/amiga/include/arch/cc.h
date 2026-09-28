/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * lwIP architecture glue for AmigaOS/m68k (bebbo gcc).
 *
 * lwIP's <stdint.h>-based defaults fit; the one thing that must not default
 * is byte order — lwip/arch.h assumes little-endian when unset, and m68k is
 * big-endian (which is also network byte order: htons/htonl become no-ops).
 */

#ifndef LWIPAMIGA_ARCH_CC_H
#define LWIPAMIGA_ARCH_CC_H

#ifndef BYTE_ORDER
#define BYTE_ORDER BIG_ENDIAN
#endif

/* Use lwIP's own ASCII character tests rather than <ctype.h>.  Mapping them to
 * ctype.h would make dns.c reference libnix's 268-byte _ctype_ table, and
 * bsdsocket.library links no other part of libnix.  The private versions are
 * also the more correct ones here: the only user is DNS label comparison, and
 * RFC 4343 case-insensitivity is defined on ASCII alone, not on a locale. */
#define LWIP_NO_CTYPE_H 1

#endif /* LWIPAMIGA_ARCH_CC_H */
