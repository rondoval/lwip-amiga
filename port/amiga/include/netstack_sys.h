/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Exec access for the port layer. Fleet convention: exec calls fetch
 * ExecBase from AbsExecBase (address 4) — no writable SysBase global needed
 * (the stack library could carry one, but staying uniform with the drivers
 * costs nothing). Include this FIRST in every port .c file.
 */

#ifndef LWIPAMIGA_NETSTACK_SYS_H
#define LWIPAMIGA_NETSTACK_SYS_H

#include <exec/libraries.h>

#define __NOLIBBASE__
#define EXEC_BASE_NAME (*(struct ExecBase **)4UL)

#ifdef __INTELLISENSE__
#include <clib/exec_protos.h>
#else
#include <proto/exec.h>
#endif

#include <exec/types.h>

#endif /* LWIPAMIGA_NETSTACK_SYS_H */
