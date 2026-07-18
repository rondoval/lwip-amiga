/* SPDX-License-Identifier: BSD-3-Clause */
/* Minimal shim replacing the "check" unit test framework's API so that
 * lwip/test/unit/tcp/tcp_helper.c can be reused in a standalone fuzzer.
 * Written from scratch — contains no code from libcheck. */
#ifndef FUZZ_CHECK_SHIM_H
#define FUZZ_CHECK_SHIM_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK_MAJOR_VERSION 0
#define CHECK_MINOR_VERSION 15
#define CHECK_MICRO_VERSION 0

typedef struct TTest { int dummy; } TTest;
typedef struct Suite Suite;
typedef struct TCase TCase;
typedef void (*TFun)(int);
typedef void (*SFun)(void);

extern void fuzz_fail_hook(const char *expr, const char *file, int line);

#define fail_unless(x) do { if (!(x)) fuzz_fail_hook(#x, __FILE__, __LINE__); } while (0)
#define fail() fuzz_fail_hook("fail()", __FILE__, __LINE__)
#define tcase_add_test(tc, tf) do { (void)(tc); } while (0)

#endif
