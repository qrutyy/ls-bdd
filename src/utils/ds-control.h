/* SPDX-License-Identifier: GPL-2.0-only */

#pragma once

#include <linux/types.h>

#define CHECK_FOR_NULL(node)					  \
	do {										  \
		if (!node)								  \
			return NULL;						  \
	} while (0)									  \

#define CHECK_VALUE_AND_RETURN(node)			  \
	do {										  \
		if (node->value)						  \
			return node->value;					  \
	} while (0)									  \

enum data_type {
	BTREE_TYPE,
	SKIPLIST_TYPE,
	HASHTABLE_TYPE,
	RBTREE_TYPE
};

struct data_struct {
	enum data_type type;
	union {
		struct btree *map_btree;
		struct skiplist *map_list;
		struct hashtable *map_hash;
		struct rbtree *map_rbtree;
	} structure;
};

int ds_init(struct data_struct *ds, char *sel_ds);
void ds_free(struct data_struct *ds, struct kmem_cache *lsbdd_hash_cache);
void *ds_lookup(struct data_struct *ds, sector_t key);
void ds_remove(struct data_struct *ds, sector_t key);
int ds_insert(struct data_struct *ds, sector_t key, void *value, struct kmem_cache *lsbdd_hash_cache);
void *ds_last(struct data_struct *ds, sector_t key);
void *ds_prev(struct data_struct *ds, sector_t key, sector_t *prev_key);
int ds_empty_check(struct data_struct *ds);

