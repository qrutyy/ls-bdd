/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef DS_CONTROL_H
#define DS_CONTROL_H

// General data structures API

#include <linux/types.h>

#define CHECK_FOR_NULL(node)                                                                                                               \
	do {                                                                                                                               \
		if (!node)                                                                                                                 \
			return NULL;                                                                                                       \
	} while (0)

#define CHECK_VALUE_AND_RETURN(node)                                                                                                       \
	do {                                                                                                                               \
		if (node->value)                                                                                                           \
			return node->value;                                                                                                \
	} while (0)

enum lsbdd_ds_type { BTREE_TYPE, SKIPLIST_TYPE, HASHTABLE_TYPE, RBTREE_TYPE };

struct lsbdd_ds {
	enum lsbdd_ds_type type;
	union {
		struct btree *map_btree;
		struct skiplist *map_list;
		struct hashtable *map_hash;
		struct rbtree *map_rbtree;
	} structure;
};

struct lsbdd_cache_mng {
	struct kmem_cache *ht_cache;
	struct kmem_cache *sl_cache;
	struct kmem_cache *bt_cache;
	struct kmem_cache *rb_cache;
};

// pretty intuitive, specific data structure methods used in ds_control.c got more detailed docs ;)

int ds_init(struct lsbdd_ds *ds, char *sel_ds, struct lsbdd_cache_mng *lsbdd_cache_mng);
void ds_free(struct lsbdd_ds *ds, struct lsbdd_cache_mng *lsbdd_cache_mng, struct kmem_cache *value_cache);
void *ds_lookup(struct lsbdd_ds *ds, sector_t key);
void ds_remove(struct lsbdd_ds *ds, sector_t key, struct kmem_cache *value_cache);
int ds_insert(struct lsbdd_ds *ds, sector_t key, void *value, struct lsbdd_cache_mng *lsbdd_cache_mng, struct kmem_cache *value_cache);
sector_t ds_last(struct lsbdd_ds *ds, sector_t key);
void *ds_prev(struct lsbdd_ds *ds, sector_t key, sector_t *prev_key);
int ds_empty_check(struct lsbdd_ds *ds);

#endif
