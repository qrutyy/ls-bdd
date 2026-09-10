/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef DS_CONTROL_H
#define DS_CONTROL_H

/* General data structures API */

#include <linux/types.h>

struct kmem_cache;

enum lsv_ds_type { BTREE_TYPE, SKIPLIST_TYPE, HASHTABLE_TYPE, RBTREE_TYPE };

struct lsv_ds {
	enum lsv_ds_type type;
	union {
		struct btree *map_btree;
		struct skiplist *map_list;
		struct hashtable *map_hash;
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
struct lsv_map_cache {
	struct lsv_cache_mng *entry_cache_mng;
	struct kmem_cache *cell_cachep;
};

s32 lsv_ds_init(struct lsv_ds *ds, char *sel_ds, struct lsv_cache_mng *cache_mng);
void lsv_ds_free(struct lsv_ds *ds, struct lsv_map_cache *map_cache);

void *lsv_ds_lookup(struct lsv_ds *ds, sector_t key);
s32 lsv_ds_insert(struct lsv_ds *ds, sector_t key, void *value, struct lsv_map_cache *map_cache);
void lsv_ds_remove(struct lsv_ds *ds, sector_t key, struct kmem_cache *cell_cachep);

sector_t lsv_ds_last(struct lsv_ds *ds, sector_t key);
void *lsv_ds_prev(struct lsv_ds *ds, sector_t key, sector_t *prev_key);
bool lsv_ds_empty_check(struct lsv_ds *ds);

bool lsv_ds_check_available(char *sel_ds);

#endif
