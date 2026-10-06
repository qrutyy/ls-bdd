/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef LSV_CORE_DEDUP_H
#define LSV_CORE_DEDUP_H

#include <linux/bio.h>
#include <linux/types.h>

#include "utils/ds_control.h"

struct lsv_dedup;
struct lsv_lmap;
struct lsv_pblk;

struct lsv_dedup_map_cache {
	struct lsv_cache_mng *entry_cache_mng;
	struct kmem_cache *de_cachep;
};

/* What lsv_dedup_write() made of a write. */
enum lsv_de_state {
	LSV_DE_NEW = 0, /* to be written to the returned pblk */
	LSV_DE_DUPLICATE, /* digest matched a stored cell: the bio now belongs to dedup */
};

/*
 * Content index entry: the digest of the data one physical cell holds. Entry
 * and pblk point at each other. The entry's link is weak, it is not counted in
 * pblk->ref: the entry is withdrawn by lsv_dedup_forget() when the last cell
 * drops the pblk.
 */
struct lsv_de {
	u64 digest;
	struct lsv_pblk *pblk;
	struct lsv_dedup *dedup; /* owning index, for lsv_dedup_forget() */
};

/* TODO(qrutyy): mb remove */
struct lsv_dedup_map {
	struct lsv_ds *ds;
};

struct lsv_dedup {
	struct lsv_dedup_map *map;
	struct lsv_lmap *lmap;
	struct block_device *bd;
};

s32 lsv_dedup_engine_init(void);
void lsv_dedup_engine_deinit(void);

struct lsv_dedup *lsv_dedup_create(struct lsv_lmap *lmap, struct block_device *bd);
void lsv_dedup_destroy(struct lsv_dedup *dedup);

s32 lsv_dedup_write(struct lsv_dedup *dedup, struct bio *bio, u64 lba, struct lsv_pblk **pblk);
void lsv_dedup_forget(struct lsv_de *de);

static inline bool lsv_de_is_new(enum lsv_de_state state)
{
	return state == LSV_DE_NEW;
}

#endif
