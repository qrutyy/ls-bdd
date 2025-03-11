// SPDX-License-Identifier: GPL-2.0-only

#include <linux/hashtable.h>
#include <linux/btree.h>
#include "ds-control.h"
#include "btree-utils.h"
#include "hashtable-utils.h"
#include "skiplist.h"
#include "rbtree.h"

s32 ds_init(struct data_struct *ds, char *sel_ds, struct cache_manager *cache_mng)
{
	struct btree *btree_map = NULL;
	struct btree_head *root = NULL;
	struct rbtree *rbtree_map = NULL;
	struct skiplist *skiplist = NULL;
	struct hashtable *hash_table = NULL;
	s32 status = 0;
	char *bt = "bt";
	char *sl = "sl";
	char *ht = "ht";
	char *rb = "rb";

	if (!strncmp(sel_ds, bt, 2)) {

		btree_map = kzalloc(sizeof(struct btree), GFP_KERNEL);
		if (!btree_map)
			goto mem_err;

		root = kzalloc(sizeof(struct btree_head), GFP_KERNEL);
		if (!root)
			goto mem_err;

		status = btree_init(root);
		if (status)
			return status;

		btree_map->head = root;
		ds->type = BTREE_TYPE;
		ds->structure.map_btree = btree_map;
	} else if (!strncmp(sel_ds, sl, 2)) {

		cache_mng->sl_cache = kmem_cache_create("skiplist_cache", sizeof(struct skiplist_node) + 24 * sizeof(struct skiplist_node *), 0, SLAB_HWCACHE_ALIGN, NULL);
		skiplist = skiplist_init(cache_mng->sl_cache);
		if (!skiplist)
			goto mem_err;

		ds->type = SKIPLIST_TYPE;
		ds->structure.map_list = skiplist;
	} else if (!strncmp(sel_ds, ht, 2)) {
		cache_mng->ht_cache = kmem_cache_create("hashtable_cache", sizeof(struct hash_el), 0, SLAB_HWCACHE_ALIGN, NULL);
		hash_table = hashtable_init(cache_mng->ht_cache);
		if (!hash_table)
			goto mem_err;

		ds->type = HASHTABLE_TYPE;
		ds->structure.map_hash = hash_table;
		ds->structure.map_hash->max_bck_num = 0;
	} else if (!strncmp(sel_ds, rb, 2)) {
		rbtree_map = kzalloc(sizeof(struct rbtree), GFP_KERNEL);
		rbtree_map = rbtree_init();
		ds->type = RBTREE_TYPE;
		ds->structure.map_rbtree = rbtree_map;
	} else {
		pr_err("Aborted. Data structure isn't choosed.\n");
		return -1;
	}
	return 0;

mem_err:
	pr_err("Memory allocation failed\n");
	kfree(ds);
	kfree(root);
	return -ENOMEM;
}

void ds_free(struct data_struct *ds, struct cache_manager *cache_mng)
{
	if (ds->type == BTREE_TYPE) {
		btree_destroy(ds->structure.map_btree->head);
		ds->structure.map_btree = NULL;
	}
	if (ds->type == SKIPLIST_TYPE) {
		skiplist_free(ds->structure.map_list, cache_mng->sl_cache);
		ds->structure.map_list = NULL;
	}
	if (ds->type == HASHTABLE_TYPE) {
		hashtable_free(ds->structure.map_hash, cache_mng->ht_cache);
		ds->structure.map_hash = NULL;
	}
	if (ds->type == RBTREE_TYPE) {
		rbtree_free(ds->structure.map_rbtree);
		ds->structure.map_rbtree = NULL;
	}
}

void *ds_lookup(struct data_struct *ds, sector_t key)
{
	struct skiplist_node *sl_node = NULL;
	struct hash_el *hm_node = NULL;
	struct rbtree_node *rb_node = NULL;
	u64 *kp = NULL;

	kp = &key;
	if (ds->type == BTREE_TYPE)
		return btree_lookup(ds->structure.map_btree->head, &btree_geo64, (unsigned long *)kp);
	if (ds->type == SKIPLIST_TYPE) {
		sl_node = skiplist_find_node(ds->structure.map_list, key);
		CHECK_FOR_NULL(sl_node);
		CHECK_VALUE_AND_RETURN(sl_node);
	}
	if (ds->type == HASHTABLE_TYPE) {
		hm_node = hashtable_find_node(ds->structure.map_hash, key);
		CHECK_FOR_NULL(hm_node);
		CHECK_VALUE_AND_RETURN(hm_node);
	}
	if (ds->type == RBTREE_TYPE) {
		rb_node = rbtree_find_node(ds->structure.map_rbtree, key);
		CHECK_FOR_NULL(rb_node);
		CHECK_VALUE_AND_RETURN(rb_node);
	}
	return NULL;
}

void ds_remove(struct data_struct *ds, sector_t key)
{
	u64 *kp = NULL;

	kp = &key;
	if (ds->type == BTREE_TYPE)
		btree_remove(ds->structure.map_btree->head, &btree_geo64, (unsigned long *)kp);
	if (ds->type == SKIPLIST_TYPE)
		skiplist_remove(ds->structure.map_list, key);
	if (ds->type == HASHTABLE_TYPE)
		hashtable_remove(ds->structure.map_hash, key);
	if (ds->type == RBTREE_TYPE)
		rbtree_remove(ds->structure.map_rbtree, key);
}

s32 ds_insert(struct data_struct *ds, sector_t key, void *value, struct cache_manager *cache_mng)
{
	u64 *kp = NULL;
	kp = &key;
	struct hash_el *el = NULL;

	if (ds->type == BTREE_TYPE)
		return btree_insert(ds->structure.map_btree->head, &btree_geo64, (unsigned long *)kp, value, GFP_KERNEL);
	if (ds->type == SKIPLIST_TYPE)
		skiplist_insert(ds->structure.map_list, key, value, cache_mng->sl_cache);
	if (ds->type == HASHTABLE_TYPE) {
		el = hashtable_insert(ds->structure.map_hash, key, value, cache_mng->ht_cache);
		if (!el)
			goto mem_err;
	}
	if (ds->type == RBTREE_TYPE)
		rbtree_add(ds->structure.map_rbtree, key, value);
	return 0;

mem_err:
	pr_err("Memory allocation failed\n");
	return -ENOMEM;
}

sector_t ds_last(struct data_struct *ds, sector_t key)
{
	struct hash_el *hm_node = NULL;
	struct rbtree_node *rb_node = NULL;
	u64 *kp = NULL;

	kp = &key;
	if (ds->type == BTREE_TYPE)
		return btree_last_no_rep(ds->structure.map_btree->head, &btree_geo64, (unsigned long *)kp);
	if (ds->type == SKIPLIST_TYPE) {
		return skiplist_last(ds->structure.map_list);
	}
	if (ds->type == HASHTABLE_TYPE) {
		hm_node = ds->structure.map_hash->last_el;
		CHECK_FOR_NULL(hm_node);
		return hm_node->key;
	}
	if (ds->type == RBTREE_TYPE) {
		rb_node = rbtree_last(ds->structure.map_rbtree);
		CHECK_FOR_NULL(rb_node);
		return rb_node->key;
	}
	pr_err("Failed to get rs_info from get_last()\n");
	BUG();
}

void *ds_prev(struct data_struct *ds, sector_t key, sector_t *prev_key)
{
	struct skiplist_node *sl_node = NULL;
	struct hash_el *hm_node = NULL;
	struct rbtree_node *rb_node = NULL;
	u64 *kp = NULL;

	kp = &key;
	if (ds->type == BTREE_TYPE)
		return btree_get_prev_no_rep(ds->structure.map_btree->head, &btree_geo64, (unsigned long *)kp, (unsigned long *)prev_key);
	if (ds->type == SKIPLIST_TYPE) {
		sl_node = skiplist_prev(ds->structure.map_list, key, prev_key);
		CHECK_FOR_NULL(sl_node);
		CHECK_VALUE_AND_RETURN(sl_node);
	}
	if (ds->type == HASHTABLE_TYPE) {
		hm_node = hashtable_prev(ds->structure.map_hash, key, prev_key);
		CHECK_FOR_NULL(hm_node);
		CHECK_VALUE_AND_RETURN(hm_node);
	}
	if (ds->type == RBTREE_TYPE) {
		rb_node = rbtree_prev(ds->structure.map_rbtree, key, prev_key);
		CHECK_FOR_NULL(rb_node);
		CHECK_VALUE_AND_RETURN(rb_node);
	}
	pr_err("Failed to get rs_info from get_prev()\n");
	BUG();
}

s32 ds_empty_check(struct data_struct *ds)
{
	if (ds->type == BTREE_TYPE && ds->structure.map_btree->head->height == 0)
		return 1;
	if (ds->type == SKIPLIST_TYPE && skiplist_is_empty(ds->structure.map_list))
		return 1;
	if (ds->type == HASHTABLE_TYPE && hashtable_is_empty(ds->structure.map_hash))
		return 1;
	if (ds->type == RBTREE_TYPE && ds->structure.map_rbtree->node_num == 0)
		return 1;
	return 0;
}

