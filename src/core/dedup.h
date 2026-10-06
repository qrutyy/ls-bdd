/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef LSV_CORE_DEDUP_H
#define LSV_CORE_DEDUP_H

#include "utils/ds_control.h"
#include <linux/bio.h>

struct lsv_dedup_map_cache {
	struct lsv_cache_mng *entry_cache_mng;
	struct kmem_cache *de_cachep;
};

enum lsv_de_state {
	LSV_DE_NEW = 0,
	LSV_DE_DUPLICATE,
};

struct lsv_de {
	u64 digest;
	struct lsv_pblk *pblk;
	enum lsv_de_state state;
};

/* TODO(qrutyy): mb remove */
struct lsv_dedup_map {
	struct lsv_ds *ds;
};

struct lsv_dedup {
	struct lsv_dedup_map *map;
};

struct lsv_de *lsv_dedup_process(struct lsv_dedup *dedup, struct bio *bio);

static inline bool lsv_de_is_new(struct lsv_de *de) {
	return de->state == LSV_DE_NEW;
}

#endif
