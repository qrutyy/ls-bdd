/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef LSV_CORE_DIGEST_H
#define LSV_CORE_DIGEST_H

#include <linux/types.h>

enum lsv_digest_alg {
	LSV_DIGEST_XXH3_64,
};

/*
 * What a digest is: the algorithm and how many bytes lsv_digest_calculate()
 * writes to |result|. Specs are static and shared, callers only hold pointers.
 */
struct lsv_digest_spec {
	enum lsv_digest_alg alg;
	const char *name;
	u32 size; /* bytes */
};

const struct lsv_digest_spec *lsv_digest_spec_get(enum lsv_digest_alg alg);

s32 lsv_digest_calculate(const struct lsv_digest_spec *spec, const void *data, size_t len, void *result);

#endif
