// SPDX-License-Identifier: GPL-2.0-only

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/string.h>

#include "core/digest.h"
#include "utils/xxh3.h"

static const struct lsv_digest_spec lsv_digest_specs[] = {
	[LSV_DIGEST_XXH3_64] = {
		.alg = LSV_DIGEST_XXH3_64,
		.name = "xxh3",
		.size = sizeof(u64),
	},
};

const struct lsv_digest_spec *lsv_digest_spec_get(enum lsv_digest_alg alg)
{
	if ((u32)alg >= ARRAY_SIZE(lsv_digest_specs))
		return NULL;

	return &lsv_digest_specs[alg];
}

/* |result| must hold spec->size bytes; no alignment is assumed. */
s32 lsv_digest_calculate(const struct lsv_digest_spec *spec, const void *data, size_t len, void *result)
{
	u64 h64;

	switch (spec->alg) {
	case LSV_DIGEST_XXH3_64:
		h64 = lsv_xxh3_64(data, len);
		memcpy(result, &h64, sizeof(h64));
		return 0;
	}

	return -EINVAL;
}
