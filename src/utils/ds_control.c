// SPDX-License-Identifier: GPL-2.0-only

#include <linux/hashtable.h>
#include <linux/btree.h>
#include "ds_control.h"
#include "btree_utils.h"
#include "skiplist.h"
#include "rbtree.h"

#ifdef LF_MODE
#include "lock-free/ht-dta/ht_dta.h"
#endif
#ifdef SY_MODE
#include "hashtable.h"
#endif

static bool lsv_ds_is_bt(char *ds)
{
	return !strncmp(ds, "bt", 2);
}

static bool lsv_ds_is_sl(char *ds)
{
	return !strncmp(ds, "sl", 2);
}

static bool lsv_ds_is_ht(char *ds)
{
	return !strncmp(ds, "ht", 2);
}

static bool lsv_ds_is_rb(char *ds)
{
	return !strncmp(ds, "rb", 2);
}

static int ds_init_type(struct lsv_ds *ds, char *sel_ds)
{
	if (lsv_ds_is_bt(sel_ds)) {
		ds->type = BTREE_TYPE;
		return 0;
	} else if (lsv_ds_is_sl(sel_ds)) {
		ds->type = SKIPLIST_TYPE;
		return 0;
	} else if (lsv_ds_is_ht(sel_ds)) {
		ds->type = HASHTABLE_TYPE;
		return 0;
	} else if (lsv_ds_is_rb(sel_ds)) {
		ds->type = RBTREE_TYPE;
		return 0;
	}

	return -EINVAL;
}

static int lsv_btree_init(struct lsv_ds *ds)
{
	struct btree *btree_map = NULL;
	struct btree_head *root = NULL;
	int rc = 0;

	btree_map = kzalloc(sizeof(struct btree), GFP_KERNEL);
	if (!btree_map)
		goto mem_err;

	root = kzalloc(sizeof(struct btree_head), GFP_KERNEL);
	if (!root)
		goto mem_err;

	rc = btree_init(root);
	if (rc)
		return rc;

	btree_map->head = root;
	ds->type = BTREE_TYPE;
	ds->structure.map_btree = btree_map;

	return rc;

mem_err:
	kfree(btree_map);
	kfree(root);
	return -ENOMEM;
}

static int lsv_skiplist_init(struct lsv_ds *ds, struct lsv_cache_mng *cache_mng)
{
	struct skiplist *skiplist = NULL;

	/* Node caches are shared by every device, so create them only once. */
	if (!cache_mng->sl_cache)
		cache_mng->sl_cache = kmem_cache_create(
			"lsv_skiplist_cache", sizeof(struct skiplist_node) + 24 * sizeof(struct skiplist_node *), 0,
			SLAB_HWCACHE_ALIGN, NULL);
	if (!cache_mng->sl_cache)
		return -ENOMEM;

	skiplist = skiplist_init(cache_mng->sl_cache);
	if (!skiplist)
		return -ENOMEM;

	ds->type = SKIPLIST_TYPE;
	ds->structure.map_list = skiplist;

	return 0;
}

#ifdef LF_MODE
/*
 * The DtA table links cells of its own, struct lsv_lf_list_node, from
 * |ht_cache|. A cell carries a value and owns none of it: node_dealloc() gives
 * back the cell alone. Values are not refcounted here, so none of the value
 * ops is set.
 */

/* Only lsv_ht_insert_or_inc_ref() asks for a cell, and ds_control never calls it. */
static struct lsv_lf_list_node *lsv_ht_node_alloc(u64 key, void *ctx)
{
	return NULL;
}

/*
 * Called by a recovery, inside a table operation that may itself run under
 * rcu_read_lock() (see lsv_dedup_lookup()), so it must not sleep. A copy it
 * cannot make is retried by a later recovery pass.
 */
static struct lsv_lf_list_node *lsv_ht_node_copy(struct lsv_lf_list_node *src, void *ctx)
{
	struct lsv_lf_list_node *copy;

	copy = kmem_cache_zalloc(ctx, GFP_NOWAIT | __GFP_NOWARN);
	if (!copy)
		return NULL;

	copy->key = src->key;
	copy->value = src->value;

	return copy;
}

static void lsv_ht_node_dealloc(struct lsv_lf_list_node *node, void *ctx)
{
	kmem_cache_free(ctx, node);
}

static const struct lsv_lf_list_ops lsv_ht_ds_ops = {
	.node_alloc = lsv_ht_node_alloc,
	.node_copy = lsv_ht_node_copy,
	.node_dealloc = lsv_ht_node_dealloc,
};

static int lsv_hashtable_init(struct lsv_ds *ds, struct lsv_cache_mng *cache_mng, u64 max_entries)
{
	struct lsv_ht *ht;

	/* one cell per entry: no alignment padding on top of its 48 bytes */
	if (!cache_mng->ht_cache)
		cache_mng->ht_cache = kmem_cache_create("lsv_hashtable_cache", sizeof(struct lsv_lf_list_node), 0, 0,
							NULL);
	if (!cache_mng->ht_cache)
		return -ENOMEM;

	ht = lsv_ht_alloc_and_init(max_entries, cache_mng->ht_cache, &lsv_ht_ds_ops);
	if (IS_ERR(ht))
		return PTR_ERR(ht);

	ds->type = HASHTABLE_TYPE;
	ds->structure.map_lsv_ht = ht;

	return 0;
}
#endif

#ifdef SY_MODE
static int lsv_hashtable_init(struct lsv_ds *ds, struct lsv_cache_mng *cache_mng, u64 max_entries)
{
	struct hashtable *hash_table = NULL;

	if (!cache_mng->ht_cache)
		cache_mng->ht_cache = kmem_cache_create("lsv_hashtable_cache", sizeof(struct hash_el), 0, SLAB_HWCACHE_ALIGN, NULL);

	if (!cache_mng->ht_cache)
		return -ENOMEM;

	hash_table = hashtable_init(cache_mng->ht_cache);
	if (!hash_table)
		return -ENOMEM;

	ds->type = HASHTABLE_TYPE;
	ds->structure.map_hash = hash_table;
	ds->structure.map_hash->max_bck_num = 0;

	return 0;
}
#endif

static int lsv_rbtree_init(struct lsv_ds *ds)
{
	struct rbtree *rbtree_map;

	rbtree_map = rbtree_init();
	if (!rbtree_map)
		return -ENOMEM;

	ds->type = RBTREE_TYPE;
	ds->structure.map_rbtree = rbtree_map;

	return 0;
}

s32 lsv_ds_init(struct lsv_ds *ds, char *sel_ds, struct lsv_cache_mng *cache_mng, u64 max_entries)
{
	s32 rc;

	BUG_ON(!ds || !cache_mng);

	rc = ds_init_type(ds, sel_ds);
	if (rc)
		return rc;

	switch (ds->type) {
	case BTREE_TYPE:
		rc = lsv_btree_init(ds);
		break;
	case SKIPLIST_TYPE:
		rc = lsv_skiplist_init(ds, cache_mng);
		break;
	case HASHTABLE_TYPE:
		rc = lsv_hashtable_init(ds, cache_mng, max_entries);
		break;
	case RBTREE_TYPE:
		rc = lsv_rbtree_init(ds);
		break;
	}

	return rc;
}

static void lsv_btree_free(struct lsv_ds *ds)
{
	btree_destroy(ds->structure.map_btree->head);
	ds->structure.map_btree = NULL;
}

static void lsv_skiplist_free(struct lsv_ds *ds, struct lsv_lmap_cache *map_cache)
{
	skiplist_free(ds->structure.map_list, map_cache->entry_cache_mng->sl_cache, map_cache->cell_cachep);
	ds->structure.map_list = NULL;
}

#ifdef LF_MODE
/*
 * Values of the cells still linked go back to |value_cachep|, as the old
 * hashtable did; the cells themselves go back through lsv_ht_free(). No two
 * linked cells share a value: a recovery copy is linked only in place of its
 * source. Values of entries removed earlier are not reachable from here any
 * more, see lsv_ds_remove().
 */
static void lsv_hashtable_free_values(struct lsv_ht *ht, struct kmem_cache *value_cachep)
{
	struct lsv_lf_list_node *node;
	u64 i;

	for (i = 0; i < ht->bucket_cnt; i++) {
		for (node = lsv_ht_cell_next(&ht->buckets[i].head); node != &ht->tail;
		     node = lsv_ht_cell_next(node))
			kmem_cache_free(value_cachep, node->value);
	}
}

static void lsv_hashtable_free(struct lsv_ds *ds, struct lsv_lmap_cache *map_cache)
{
	lsv_hashtable_free_values(ds->structure.map_lsv_ht, map_cache->cell_cachep);
	lsv_ht_free(ds->structure.map_lsv_ht);
	ds->structure.map_lsv_ht = NULL;
}
#endif

#ifdef SY_MODE
static void lsv_hashtable_free(struct lsv_ds *ds, struct lsv_lmap_cache *map_cache)
{
	hashtable_free(ds->structure.map_hash, map_cache->entry_cache_mng->ht_cache, map_cache->cell_cachep);
	ds->structure.map_hash = NULL;
}
#endif

static void lsv_rbtree_free(struct lsv_ds *ds)
{
	rbtree_free(ds->structure.map_rbtree);
	ds->structure.map_rbtree = NULL;
}

void lsv_ds_free(struct lsv_ds *ds, struct lsv_lmap_cache *map_cache)
{
	BUG_ON(!ds || !map_cache);

	switch (ds->type) {
	case BTREE_TYPE:
		lsv_btree_free(ds);
		break;
	case SKIPLIST_TYPE:
		lsv_skiplist_free(ds, map_cache);
		break;
	case HASHTABLE_TYPE:
		lsv_hashtable_free(ds, map_cache);
		break;
	case RBTREE_TYPE:
		lsv_rbtree_free(ds);
		break;
	}
}

static void *lsv_btree_lookup(struct lsv_ds *ds, sector_t key)
{
	u64 *kp = NULL;

	kp = &key;

	return btree_lookup(ds->structure.map_btree->head, &btree_geo64, (unsigned long *)kp);
}

static void *lsv_skiplist_lookup(struct lsv_ds *ds, sector_t key)
{
	struct skiplist_node *sl_node = NULL;

	sl_node = skiplist_find_node(ds->structure.map_list, key);
	if (!sl_node || !sl_node->value)
		return NULL;

	return sl_node->value;
}

#ifdef LF_MODE
static void *lsv_ht_lookup(struct lsv_ds *ds, sector_t key)
{
	return lsv_ht_find_value(ds->structure.map_lsv_ht, key);
}
#endif

#ifdef SY_MODE
static void *lsv_ht_lookup(struct lsv_ds *ds, sector_t key)
{
	struct hash_el *hm_node = NULL;

	hm_node = hashtable_find_node(ds->structure.map_hash, key);
	if (!hm_node || !hm_node->value)
		return NULL;

	return hm_node->value;
}
#endif

static void *lsv_rbtree_lookup(struct lsv_ds *ds, sector_t key)
{
	struct rbtree_node *rb_node = NULL;

	rb_node = rbtree_find_node(ds->structure.map_rbtree, key);
	if (!rb_node || !rb_node->value)
		return NULL;

	return rb_node->value;
}

void *lsv_ds_lookup(struct lsv_ds *ds, sector_t key)
{
	BUG_ON(!ds);

	switch (ds->type) {
	case BTREE_TYPE:
		return lsv_btree_lookup(ds, key);
	case SKIPLIST_TYPE:
		return lsv_skiplist_lookup(ds, key);
	case HASHTABLE_TYPE:
		return lsv_ht_lookup(ds, key);
	case RBTREE_TYPE:
		return lsv_rbtree_lookup(ds, key);
	}

	return NULL;
}

void lsv_ds_remove(struct lsv_ds *ds, sector_t key, struct kmem_cache *lsv_value_cache)
{
	BUG_ON(!ds || !lsv_value_cache);

	u64 *kp = NULL;

	kp = &key;
	switch (ds->type) {
	case BTREE_TYPE:
		btree_remove(ds->structure.map_btree->head, &btree_geo64, (unsigned long *)kp);
		break;
	case SKIPLIST_TYPE:
		skiplist_remove(ds->structure.map_list, key, lsv_value_cache);
		break;
	case HASHTABLE_TYPE:
#ifdef LF_MODE
		/*
		 * The value is left to the caller, as the old hashtable left it
		 * until the table was freed: a concurrent lookup may still hand
		 * it out. TODO: free it once the table counts value references.
		 */
		lsv_ht_remove(ds->structure.map_lsv_ht, key);
#endif
#ifdef SY_MODE
		hashtable_remove(ds->structure.map_hash, key, lsv_value_cache);
#endif
		break;
	case RBTREE_TYPE:
		rbtree_remove(ds->structure.map_rbtree, key);
		break;
	}
}

static s32 lsv_btree_insert(struct lsv_ds *ds, sector_t key, void *value)
{
	u64 *kp = &key;

	return btree_insert(ds->structure.map_btree->head, &btree_geo64, (unsigned long *)kp, value, GFP_KERNEL);
}

static s32 lsv_skiplist_insert(struct lsv_ds *ds, sector_t key, void *value, struct lsv_lmap_cache *map_cache)
{
	skiplist_insert(ds->structure.map_list, key, value, map_cache->entry_cache_mng->sl_cache, map_cache->cell_cachep);

	return 0;
}

#ifdef LF_MODE
/*
 * A key that is already there keeps its entry, and |value| goes back to
 * |cell_cachep|, as the old hashtable did: callers tell the two apart by
 * looking the key up again. A failure leaves |value| with the caller.
 */
static s32 lsv_hashtable_insert(struct lsv_ds *ds, sector_t key, void *value, struct lsv_lmap_cache *map_cache)
{
	struct kmem_cache *cachep = map_cache->entry_cache_mng->ht_cache;
	struct lsv_lf_list_node *node;
	struct lsv_lf_list_node *cell;

	if (lsv_ht_key_is_reserved(key))
		return -EINVAL;

	node = kmem_cache_zalloc(cachep, GFP_NOIO);
	if (!node)
		return -ENOMEM;

	node->key = key;
	node->value = value;

	cell = lsv_ht_insert_node(ds->structure.map_lsv_ht, key, node);
	if (cell == node)
		return 0;

	kmem_cache_free(cachep, node);

	/* a spurious failure, or a caller that is not a participant of the table */
	if (!cell)
		return -EAGAIN;

	kmem_cache_free(map_cache->cell_cachep, value);
	return 0;
}
#endif

#ifdef SY_MODE
static s32 lsv_hashtable_insert(struct lsv_ds *ds, sector_t key, void *value, struct lsv_lmap_cache *map_cache)
{
	hashtable_insert(ds->structure.map_hash, key, value, map_cache->entry_cache_mng->ht_cache, map_cache->cell_cachep);

	return 0;
}
#endif

static s32 lsv_rbtree_insert(struct lsv_ds *ds, sector_t key, void *value)
{
	rbtree_add(ds->structure.map_rbtree, key, value);

	return 0;
}

s32 lsv_ds_insert(struct lsv_ds *ds, sector_t key, void *value, struct lsv_lmap_cache *map_cache)
{
	BUG_ON(!ds || !map_cache);

	switch (ds->type) {
	case BTREE_TYPE:
		return lsv_btree_insert(ds, key, value);
	case SKIPLIST_TYPE:
		return lsv_skiplist_insert(ds, key, value, map_cache);
	case HASHTABLE_TYPE:
		return lsv_hashtable_insert(ds, key, value, map_cache);
	case RBTREE_TYPE:
		return lsv_rbtree_insert(ds, key, value);
	}

	return -EINVAL;
}

static sector_t lsv_btree_last(struct lsv_ds *ds, sector_t key)
{
	u64 *kp = &key;

	return btree_last_no_rep(ds->structure.map_btree->head, &btree_geo64, (unsigned long *)kp);
}

static sector_t lsv_skiplist_last(struct lsv_ds *ds)
{
	return skiplist_last(ds->structure.map_list);
}

#ifdef LF_MODE
/* The DtA table keeps no order across buckets and no last element. */
static sector_t lsv_hashtable_last(struct lsv_ds *ds)
{
	return 0;
}
#endif

#ifdef SY_MODE
static sector_t lsv_hashtable_last(struct lsv_ds *ds)
{
	struct hash_el *hm_node = NULL;

	hm_node = ds->structure.map_hash->last_el;
	if (!hm_node)
		return 0;

	return hm_node->key;
}
#endif

static sector_t lsv_rbtree_last(struct lsv_ds *ds)
{
	struct rbtree_node *rb_node = NULL;

	rb_node = rbtree_last(ds->structure.map_rbtree);
	if (!rb_node)
		return 0;

	return rb_node->key;
}

sector_t lsv_ds_last(struct lsv_ds *ds, sector_t key)
{
	BUG_ON(!ds);

	switch (ds->type) {
	case BTREE_TYPE:
		return lsv_btree_last(ds, key);
	case SKIPLIST_TYPE:
		return lsv_skiplist_last(ds);
	case HASHTABLE_TYPE:
		return lsv_hashtable_last(ds);
	case RBTREE_TYPE:
		return lsv_rbtree_last(ds);
	}

	return 0;
}

static void *lsv_btree_prev(struct lsv_ds *ds, sector_t key, sector_t *prev_key)
{
	u64 *kp = &key;

	return btree_get_prev_no_rep(ds->structure.map_btree->head, &btree_geo64, (unsigned long *)kp, (unsigned long *)prev_key);
}

static void *lsv_skiplist_prev(struct lsv_ds *ds, sector_t key, sector_t *prev_key)
{
	struct skiplist_node *sl_node = NULL;

	sl_node = skiplist_prev(ds->structure.map_list, key, prev_key);
	if (!sl_node)
		return NULL;

	return sl_node->value;
}

#ifdef LF_MODE
/* No order across buckets, so no predecessor either. */
static void *lsv_hashtable_prev(struct lsv_ds *ds, sector_t key, sector_t *prev_key)
{
	return NULL;
}
#endif

#ifdef SY_MODE
static void *lsv_hashtable_prev(struct lsv_ds *ds, sector_t key, sector_t *prev_key)
{
	struct hash_el *hm_node = NULL;

	hm_node = hashtable_prev(ds->structure.map_hash, key, prev_key);
	if (!hm_node)
		return NULL;

	return hm_node->value;
}
#endif

static void *lsv_rbtree_prev(struct lsv_ds *ds, sector_t key, sector_t *prev_key)
{
	struct rbtree_node *rb_node = NULL;

	rb_node = rbtree_prev(ds->structure.map_rbtree, key, prev_key);
	if (!rb_node)
		return NULL;

	return rb_node->value;
}

void *lsv_ds_prev(struct lsv_ds *ds, sector_t key, sector_t *prev_key)
{
	BUG_ON(!ds || !prev_key);

	switch (ds->type) {
	case BTREE_TYPE:
		return lsv_btree_prev(ds, key, prev_key);
	case SKIPLIST_TYPE:
		return lsv_skiplist_prev(ds, key, prev_key);
	case HASHTABLE_TYPE:
		return lsv_hashtable_prev(ds, key, prev_key);
	case RBTREE_TYPE:
		return lsv_rbtree_prev(ds, key, prev_key);
	}

	return NULL;
}

static bool lsv_btree_empty(struct lsv_ds *ds)
{
	return ds->structure.map_btree->head->height == 0;
}

static bool lsv_skiplist_empty(struct lsv_ds *ds)
{
	return skiplist_is_empty(ds->structure.map_list);
}

static bool lsv_hashtable_empty(struct lsv_ds *ds)
{
#ifdef LF_MODE
	return lsv_ht_is_empty(ds->structure.map_lsv_ht);
#endif
#ifdef SY_MODE
	return hashtable_is_empty(ds->structure.map_hash);
#endif
}

static bool lsv_rbtree_empty(struct lsv_ds *ds)
{
	return ds->structure.map_rbtree->node_num == 0;
}

bool lsv_ds_empty_check(struct lsv_ds *ds)
{
	BUG_ON(!ds);

	switch (ds->type) {
	case BTREE_TYPE:
		return lsv_btree_empty(ds);
	case SKIPLIST_TYPE:
		return lsv_skiplist_empty(ds);
	case HASHTABLE_TYPE:
		return lsv_hashtable_empty(ds);
	case RBTREE_TYPE:
		return lsv_rbtree_empty(ds);
	}

	return true;
}

bool lsv_ds_check_available(char *sel_ds)
{
	if (!sel_ds)
		return false;

	return lsv_ds_is_bt(sel_ds) || lsv_ds_is_sl(sel_ds) || lsv_ds_is_ht(sel_ds) || lsv_ds_is_rb(sel_ds);
}
