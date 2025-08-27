// SPDX-License-Identifier: GPL-2.0-only

#include <linux/hashtable.h>
#include <linux/btree.h>
#include "ds_control.h"
#include "btree_utils.h"
#include "hashtable.h"
#include "skiplist.h"
#include "rbtree.h"

#ifdef LF_MODE
#include "lf_list.h"
#endif


s32 ds_init(struct lsbdd_ds *ds, char *sel_ds, struct lsbdd_cache_mng *cache_mng)
{
	BUG_ON(!ds || !cache_mng);

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
		cache_mng->sl_cache = kmem_cache_create(
			"lsbdd_skiplist_cache", sizeof(struct skiplist_node) + 24 * sizeof(struct skiplist_node *), 0, SLAB_HWCACHE_ALIGN, NULL);
		// TODO docs
		if (!cache_mng->sl_cache) {
			pr_err("ERROR DS_INIT: skiplist cache not initialized!\n");
			return -1;
		}
		skiplist = skiplist_init(cache_mng->sl_cache);
		if (!skiplist)
			goto mem_err;

		ds->type = SKIPLIST_TYPE;
		ds->structure.map_list = skiplist;
	} else if (!strncmp(sel_ds, ht, 2)) {
		#ifdef LF_MODE
		cache_mng->ht_cache = kmem_cache_create("lsbdd_hashtable_cache", sizeof(struct lf_list_node), 0, SLAB_HWCACHE_ALIGN, NULL);
		#endif
		#ifdef SY_MODE
		cache_mng->ht_cache = kmem_cache_create("lsbdd_hashtable_cache", sizeof(struct hash_el), 0, SLAB_HWCACHE_ALIGN, NULL);
		#endif
		if (!cache_mng->ht_cache) {
			pr_err("ERROR DS_INIT: hastable cache not initialized!\n");
			return -1;
		}

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

void ds_free(struct lsbdd_ds *ds, struct lsbdd_cache_mng *cache_mng, struct kmem_cache *lsbdd_value_cache)
{
	BUG_ON(!ds || !cache_mng || !lsbdd_value_cache);

	switch (ds->type) {
	case BTREE_TYPE:
		btree_destroy(ds->structure.map_btree->head);
		ds->structure.map_btree = NULL;
		break;
	case SKIPLIST_TYPE:
		skiplist_free(ds->structure.map_list, cache_mng->sl_cache, lsbdd_value_cache);
		ds->structure.map_list = NULL;
		break;
	case HASHTABLE_TYPE:
		hashtable_free(ds->structure.map_hash, cache_mng->ht_cache, lsbdd_value_cache);
		ds->structure.map_hash = NULL;
		break;
	case RBTREE_TYPE:
		rbtree_free(ds->structure.map_rbtree);
		ds->structure.map_rbtree = NULL;
		break;
	}
}

void *ds_lookup(struct lsbdd_ds *ds, sector_t key)
{
	BUG_ON(!ds);

	struct skiplist_node *sl_node = NULL;
	#ifdef LF_MODE
	struct lf_list_node *hm_node = NULL;
	#endif
	#ifdef SY_MODE
	struct hash_el *hm_node = NULL;
	#endif
	struct rbtree_node *rb_node = NULL;
	u64 *kp = NULL;

	kp = &key;
	switch (ds->type) {
	case BTREE_TYPE:
		return btree_lookup(ds->structure.map_btree->head, &btree_geo64, (unsigned long *)kp);
	case SKIPLIST_TYPE:
		sl_node = skiplist_find_node(ds->structure.map_list, key);
		CHECK_FOR_NULL(sl_node);
		CHECK_VALUE_AND_RETURN(sl_node);
		break;
	case HASHTABLE_TYPE:
		hm_node = hashtable_find_node(ds->structure.map_hash, key);
		CHECK_FOR_NULL(hm_node);
		CHECK_VALUE_AND_RETURN(hm_node);
		break;
	case RBTREE_TYPE:
		rb_node = rbtree_find_node(ds->structure.map_rbtree, key);
		CHECK_FOR_NULL(rb_node);
		CHECK_VALUE_AND_RETURN(rb_node);
		break;
	}
	return NULL;
}

void ds_remove(struct lsbdd_ds *ds, sector_t key, struct kmem_cache *lsbdd_value_cache)
{
	BUG_ON(!ds || !lsbdd_value_cache);

	u64 *kp = NULL;

	kp = &key;
	switch (ds->type) {
	case BTREE_TYPE:
		btree_remove(ds->structure.map_btree->head, &btree_geo64, (unsigned long *)kp);
		break;
	case SKIPLIST_TYPE:
		skiplist_remove(ds->structure.map_list, key, lsbdd_value_cache);
		break;
	case HASHTABLE_TYPE:
		hashtable_remove(ds->structure.map_hash, key, lsbdd_value_cache);
		break;
	case RBTREE_TYPE:
		rbtree_remove(ds->structure.map_rbtree, key);
		break;
	}
}

s32 ds_insert(struct lsbdd_ds *ds, sector_t key, void *value, struct lsbdd_cache_mng *cache_mng, struct kmem_cache *lsbdd_value_cache)
{
	BUG_ON(!ds || !cache_mng || !lsbdd_value_cache);
	u64 *kp = NULL;

	kp = &key;
	switch (ds->type) {
	case  BTREE_TYPE:
		return btree_insert(ds->structure.map_btree->head, &btree_geo64, (unsigned long *)kp, value, GFP_KERNEL);
		break;
	case SKIPLIST_TYPE:
		skiplist_insert(ds->structure.map_list, key, value, cache_mng->sl_cache, lsbdd_value_cache);
		break;
	case HASHTABLE_TYPE:
		hashtable_insert(ds->structure.map_hash, key, value, cache_mng->ht_cache, lsbdd_value_cache);
		break;
	case RBTREE_TYPE:
		rbtree_add(ds->structure.map_rbtree, key, value);
		break;
	}
	return 0;
}

sector_t ds_last(struct lsbdd_ds *ds, sector_t key)
{
	BUG_ON(!ds);
	#ifdef LF_MODE
	struct lf_list_node *hm_node = NULL;
#endif
	#ifdef SY_MODE
	struct hash_el *hm_node = NULL;
	#endif
	struct rbtree_node *rb_node = NULL;
	u64 *kp = NULL;

	kp = &key;
	switch (ds->type) {
	case BTREE_TYPE:
		return btree_last_no_rep(ds->structure.map_btree->head, &btree_geo64, (unsigned long *)kp);
		break;
	case SKIPLIST_TYPE:
		return skiplist_last(ds->structure.map_list);
		break;
	case HASHTABLE_TYPE:
		hm_node = ds->structure.map_hash->last_el;
		if (hm_node == NULL)
			return 0;
		return hm_node->key;
	case RBTREE_TYPE:
		rb_node = rbtree_last(ds->structure.map_rbtree);
		if (rb_node == NULL)
			return 0;
		return rb_node->key;
	}
	pr_err("Failed to get rs_info from get_last()\n");
	BUG();
}

void *ds_prev(struct lsbdd_ds *ds, sector_t key, sector_t *prev_key)
{
	BUG_ON(!ds);

	struct skiplist_node *sl_node = NULL;
#ifdef LF_MODE
	struct lf_list_node *hm_node = NULL;
#endif
	#ifdef SY_MODE
	struct hash_el *hm_node = NULL;
	#endif
	struct rbtree_node *rb_node = NULL;
	u64 *kp = NULL;

	kp = &key;
	switch (ds->type) {
	case BTREE_TYPE:
		return btree_get_prev_no_rep(ds->structure.map_btree->head, &btree_geo64, (unsigned long *)kp, (unsigned long *)prev_key);
		break;
	case SKIPLIST_TYPE:
		sl_node = skiplist_prev(ds->structure.map_list, key, prev_key);
		CHECK_FOR_NULL(sl_node);
		CHECK_VALUE_AND_RETURN(sl_node);
		break;
	case HASHTABLE_TYPE:
		hm_node = hashtable_prev(ds->structure.map_hash, key, prev_key);
		CHECK_FOR_NULL(hm_node);
		CHECK_VALUE_AND_RETURN(hm_node);
		break;
	case RBTREE_TYPE:
		rb_node = rbtree_prev(ds->structure.map_rbtree, key, prev_key);
		CHECK_FOR_NULL(rb_node);
		CHECK_VALUE_AND_RETURN(rb_node);
		break;
	}
	pr_err("Failed to get rs_info from get_prev()\n");
	BUG();
}

s32 ds_empty_check(struct lsbdd_ds *ds)
{
	BUG_ON(!ds);

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
