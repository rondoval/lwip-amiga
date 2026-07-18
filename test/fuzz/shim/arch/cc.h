/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef FUZZ_ARCH_CC_H
#define FUZZ_ARCH_CC_H

#include <stdio.h>
#include <stdlib.h>

extern void fuzz_platform_assert(const char *msg, const char *file, int line);

#define LWIP_PLATFORM_DIAG(x) do { printf x; } while (0)
#define LWIP_PLATFORM_ASSERT(x) fuzz_platform_assert(x, __FILE__, __LINE__)

#define LWIP_RAND() ((u32_t)rand())

#endif
