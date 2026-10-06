/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef LSV_UTILS_XXH3_H
#define LSV_UTILS_XXH3_H

#include <linux/types.h>

/*
 * XXH3-64 with the default secret and a zero seed, i.e. the value upstream
 * XXH3_64bits() returns. The kernel only ships xxh32/xxh64, hence this scalar
 * port; no SIMD, so it is safe in any context without kernel_fpu_begin().
 */
u64 lsv_xxh3_64(const void *data, size_t len);

#endif
