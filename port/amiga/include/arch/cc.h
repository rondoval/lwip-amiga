/* SPDX-License-Identifier: GPL-2.0-or-later */
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

#endif /* LWIPAMIGA_ARCH_CC_H */
