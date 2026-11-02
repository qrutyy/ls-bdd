/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef DS_CONTROL_H
#define DS_CONTROL_H

/* General data structures API */

#include <linux/bits.h>
#include <linux/limits.h>
#include <linux/types.h>

struct kmem_cache;

struct lsv_ds_ops;

struct lsv_ds {
	const struct lsv_ds_ops *ops;
	union {
		struct btree *map_btree;
		struct skiplist *map_list;
		struct hashtable *map_hash;
		struct lsv_ht *map_lsv_ht;
		struct rbtree *map_rbtree;
	} structure;
};

/* Caches the index implementations allocate their nodes from. */
struct lsv_cache_mng {
	struct kmem_cache *ht_cache;
	struct kmem_cache *sl_cache;
	struct kmem_cache *bt_cache;
	struct kmem_cache *rb_cache;
};

/* Node caches plus the cache the mapping cells themselves come from. */
/* TODO(qrutyy): make it generic so every caller can pass it. think about verification */
struct lsv_lmap_cache {
	struct lsv_cache_mng *entry_cache_mng;
	struct kmem_cache *cell_cachep;
};

/*
 * What the core asks of an index. Every implementation registers one of these
 * in ds_control.c and is from then on found by name: by configfs, by lmap and
 * dedup, and by the contract and benchmark KUnit suites, which run against
 * every registered index without knowing it.
 *
 * Indexes differ in who owns the values, so each says what it does:
 *
 * LSV_DS_FREE_OWNS_VALUES	free() releases the values still mapped, to
 *				map_cache->cell_cachep (or kfree()); otherwise
 *				they stay the caller's.
 * LSV_DS_REMOVE_OWNS_VALUE	remove() releases the removed value.
 * LSV_DS_ORDERED		last() and prev() follow key order; otherwise
 *				they return 0 and NULL.
 * LSV_DS_REMOVE_ABSENT_OK	remove() of a key that is not there is a no-op;
 *				otherwise callers must only remove mapped keys.
 *
 * Keys are never reserved ones, see lsv_ds_key_is_reserved(). Inserting a key
 * that is already mapped is not part of the contract: lmap and dedup look a
 * key up first, and indexes disagree on what a second insertion does.
 */
#define LSV_DS_FREE_OWNS_VALUES BIT(0)
#define LSV_DS_REMOVE_OWNS_VALUE BIT(1)
#define LSV_DS_ORDERED BIT(2)
#define LSV_DS_REMOVE_ABSENT_OK BIT(3)

struct lsv_ds_ops {
	const char *name;
	u32 caps;

	/* |max_entries| sizes bounded indexes (the lock-free hashtable); others ignore it. */
	s32 (*init)(struct lsv_ds *ds, struct lsv_cache_mng *cache_mng, u64 max_entries);
	void (*free)(struct lsv_ds *ds, struct lsv_lmap_cache *map_cache);

	void *(*lookup)(struct lsv_ds *ds, sector_t key);
	s32 (*insert)(struct lsv_ds *ds, sector_t key, void *value, struct lsv_lmap_cache *map_cache);
	void (*remove)(struct lsv_ds *ds, sector_t key, struct kmem_cache *cell_cachep);

	sector_t (*last)(struct lsv_ds *ds, sector_t key);
	void *(*prev)(struct lsv_ds *ds, sector_t key, sector_t *prev_key);
	bool (*empty)(struct lsv_ds *ds);
};

/* The registry, in registration order; NULL past the end. */
const struct lsv_ds_ops *lsv_ds_ops_at(u32 idx);
const struct lsv_ds_ops *lsv_ds_find(const char *name);

s32 lsv_ds_init(struct lsv_ds *ds, const char *sel_ds, struct lsv_cache_mng *cache_mng, u64 max_entries);
void lsv_ds_free(struct lsv_ds *ds, struct lsv_lmap_cache *map_cache);

void *lsv_ds_lookup(struct lsv_ds *ds, sector_t key);
s32 lsv_ds_insert(struct lsv_ds *ds, sector_t key, void *value, struct lsv_lmap_cache *map_cache);
void lsv_ds_remove(struct lsv_ds *ds, sector_t key, struct kmem_cache *cell_cachep);

sector_t lsv_ds_last(struct lsv_ds *ds, sector_t key);
void *lsv_ds_prev(struct lsv_ds *ds, sector_t key, sector_t *prev_key);
bool lsv_ds_empty_check(struct lsv_ds *ds);

bool lsv_ds_check_available(const char *sel_ds);

/*
 * Keys no index is guaranteed to take: the hashtables keep their head and tail
 * sentinels under them and refuse to insert them.
 */
static inline bool lsv_ds_key_is_reserved(sector_t key)
{
	return key == 0 || key == U64_MAX;
}

#endif
