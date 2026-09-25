/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Exec access for the port layer.
 * Include this FIRST in every port .c file.
 */

#ifndef LWIPAMIGA_NETSTACK_SYS_H
#define LWIPAMIGA_NETSTACK_SYS_H

#include <exec/libraries.h>

#define __NOLIBBASE__
#define EXEC_BASE_NAME SysBase

#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#else
#include <proto/exec.h>
#endif

#include <exec/types.h>

extern struct ExecBase *SysBase; /* defined in src/bsdsocket/main.c */

#endif /* LWIPAMIGA_NETSTACK_SYS_H */
