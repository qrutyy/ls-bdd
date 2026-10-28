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

static s32 lsv_btree_init(struct lsv_ds *ds, struct lsv_cache_mng *cache_mng, u64 max_entries)
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
	ds->structure.map_btree = btree_map;

	return rc;

mem_err:
	kfree(btree_map);
	kfree(root);
	return -ENOMEM;
}

static s32 lsv_skiplist_init(struct lsv_ds *ds, struct lsv_cache_mng *cache_mng, u64 max_entries)
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

static s32 lsv_hashtable_init(struct lsv_ds *ds, struct lsv_cache_mng *cache_mng, u64 max_entries)
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

	ds->structure.map_lsv_ht = ht;

	return 0;
}
#endif

#ifdef SY_MODE
static s32 lsv_hashtable_init(struct lsv_ds *ds, struct lsv_cache_mng *cache_mng, u64 max_entries)
{
	struct hashtable *hash_table = NULL;

	if (!cache_mng->ht_cache)
		cache_mng->ht_cache = kmem_cache_create("lsv_hashtable_cache", sizeof(struct hash_el), 0, SLAB_HWCACHE_ALIGN, NULL);

	if (!cache_mng->ht_cache)
		return -ENOMEM;

	hash_table = hashtable_init(cache_mng->ht_cache);
	if (!hash_table)
		return -ENOMEM;

	ds->structure.map_hash = hash_table;
	ds->structure.map_hash->max_bck_num = 0;

	return 0;
}
#endif

static s32 lsv_rbtree_init(struct lsv_ds *ds, struct lsv_cache_mng *cache_mng, u64 max_entries)
{
	struct rbtree *rbtree_map;

	rbtree_map = rbtree_init();
	if (!rbtree_map)
		return -ENOMEM;

	ds->structure.map_rbtree = rbtree_map;

	return 0;
}

static void lsv_btree_free(struct lsv_ds *ds, struct lsv_lmap_cache *map_cache)
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

static void lsv_rbtree_free(struct lsv_ds *ds, struct lsv_lmap_cache *map_cache)
{
	rbtree_free(ds->structure.map_rbtree);
	ds->structure.map_rbtree = NULL;
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

static void lsv_btree_remove(struct lsv_ds *ds, sector_t key, struct kmem_cache *cell_cachep)
{
	u64 *kp = &key;

	btree_remove(ds->structure.map_btree->head, &btree_geo64, (unsigned long *)kp);
}

static void lsv_skiplist_remove(struct lsv_ds *ds, sector_t key, struct kmem_cache *cell_cachep)
{
	skiplist_remove(ds->structure.map_list, key, cell_cachep);
}

#ifdef LF_MODE
/*
 * The value is left to the caller, as the old hashtable left it until the
 * table was freed: a concurrent lookup may still hand it out. TODO: free it
 * once the table counts value references.
 */
static void lsv_hashtable_remove(struct lsv_ds *ds, sector_t key, struct kmem_cache *cell_cachep)
{
	lsv_ht_remove(ds->structure.map_lsv_ht, key);
}
#endif

#ifdef SY_MODE
static void lsv_hashtable_remove(struct lsv_ds *ds, sector_t key, struct kmem_cache *cell_cachep)
{
	hashtable_remove(ds->structure.map_hash, key, cell_cachep);
}
#endif

static void lsv_rbtree_remove(struct lsv_ds *ds, sector_t key, struct kmem_cache *cell_cachep)
{
	rbtree_remove(ds->structure.map_rbtree, key);
}

static s32 lsv_btree_insert(struct lsv_ds *ds, sector_t key, void *value, struct lsv_lmap_cache *map_cache)
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

static s32 lsv_rbtree_insert(struct lsv_ds *ds, sector_t key, void *value, struct lsv_lmap_cache *map_cache)
{
	rbtree_add(ds->structure.map_rbtree, key, value);

	return 0;
}

static sector_t lsv_btree_last(struct lsv_ds *ds, sector_t key)
{
	u64 *kp = &key;

	return btree_last_no_rep(ds->structure.map_btree->head, &btree_geo64, (unsigned long *)kp);
}

static sector_t lsv_skiplist_last(struct lsv_ds *ds, sector_t key)
{
	return skiplist_last(ds->structure.map_list);
}

#ifdef LF_MODE
/* The DtA table keeps no order across buckets and no last element. */
static sector_t lsv_hashtable_last(struct lsv_ds *ds, sector_t key)
{
	return 0;
}
#endif

#ifdef SY_MODE
static sector_t lsv_hashtable_last(struct lsv_ds *ds, sector_t key)
{
	struct hash_el *hm_node = NULL;

	hm_node = ds->structure.map_hash->last_el;
	if (!hm_node)
		return 0;

	return hm_node->key;
}
#endif

static sector_t lsv_rbtree_last(struct lsv_ds *ds, sector_t key)
{
	struct rbtree_node *rb_node = NULL;

	rb_node = rbtree_last(ds->structure.map_rbtree);
	if (!rb_node)
		return 0;

	return rb_node->key;
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

static const struct lsv_ds_ops lsv_bt_ops = {
	.name = "bt",
	.caps = LSV_DS_ORDERED | LSV_DS_REMOVE_ABSENT_OK,
	.init = lsv_btree_init,
	.free = lsv_btree_free,
	.lookup = lsv_btree_lookup,
	.insert = lsv_btree_insert,
	.remove = lsv_btree_remove,
	.last = lsv_btree_last,
	.prev = lsv_btree_prev,
	.empty = lsv_btree_empty,
};

static const struct lsv_ds_ops lsv_sl_ops = {
	.name = "sl",
#ifdef LF_MODE
	.caps = LSV_DS_FREE_OWNS_VALUES | LSV_DS_REMOVE_OWNS_VALUE | LSV_DS_ORDERED | LSV_DS_REMOVE_ABSENT_OK,
#else
	.caps = LSV_DS_ORDERED | LSV_DS_REMOVE_ABSENT_OK,
#endif
	.init = lsv_skiplist_init,
	.free = lsv_skiplist_free,
	.lookup = lsv_skiplist_lookup,
	.insert = lsv_skiplist_insert,
	.remove = lsv_skiplist_remove,
	.last = lsv_skiplist_last,
	.prev = lsv_skiplist_prev,
	.empty = lsv_skiplist_empty,
};

static const struct lsv_ds_ops lsv_ht_ops = {
	.name = "ht",
#ifdef LF_MODE
	.caps = LSV_DS_FREE_OWNS_VALUES | LSV_DS_REMOVE_ABSENT_OK,
#else
	/* hashtable_remove() dereferences what it did not find */
	.caps = 0,
#endif
	.init = lsv_hashtable_init,
	.free = lsv_hashtable_free,
	.lookup = lsv_ht_lookup,
	.insert = lsv_hashtable_insert,
	.remove = lsv_hashtable_remove,
	.last = lsv_hashtable_last,
	.prev = lsv_hashtable_prev,
	.empty = lsv_hashtable_empty,
};

static const struct lsv_ds_ops lsv_rb_ops = {
	.name = "rb",
	/* rbtree frees values with kfree(), which takes slab objects too */
	.caps = LSV_DS_FREE_OWNS_VALUES | LSV_DS_REMOVE_OWNS_VALUE | LSV_DS_ORDERED | LSV_DS_REMOVE_ABSENT_OK,
	.init = lsv_rbtree_init,
	.free = lsv_rbtree_free,
	.lookup = lsv_rbtree_lookup,
	.insert = lsv_rbtree_insert,
	.remove = lsv_rbtree_remove,
	.last = lsv_rbtree_last,
	.prev = lsv_rbtree_prev,
	.empty = lsv_rbtree_empty,
};

/*
 * Every index the driver knows. A new one is added here, and nowhere else in
 * the core: configfs, lmap, dedup and the KUnit suites go through this table.
 */
static const struct lsv_ds_ops *const lsv_ds_registry[] = {
	&lsv_bt_ops,
	&lsv_sl_ops,
	&lsv_ht_ops,
	&lsv_rb_ops,
};

const struct lsv_ds_ops *lsv_ds_ops_at(u32 idx)
{
	return idx < ARRAY_SIZE(lsv_ds_registry) ? lsv_ds_registry[idx] : NULL;
}

const struct lsv_ds_ops *lsv_ds_find(const char *name)
{
	u32 i;

	if (!name)
		return NULL;

	for (i = 0; i < ARRAY_SIZE(lsv_ds_registry); i++)
		if (!strcmp(lsv_ds_registry[i]->name, name))
			return lsv_ds_registry[i];

	return NULL;
}

bool lsv_ds_check_available(const char *sel_ds)
{
	return lsv_ds_find(sel_ds);
}

s32 lsv_ds_init(struct lsv_ds *ds, const char *sel_ds, struct lsv_cache_mng *cache_mng, u64 max_entries)
{
	const struct lsv_ds_ops *ops;
	s32 rc;

	BUG_ON(!ds || !cache_mng);

	ops = lsv_ds_find(sel_ds);
	if (!ops)
		return -EINVAL;

	rc = ops->init(ds, cache_mng, max_entries);
	if (rc)
		return rc;

	ds->ops = ops;

	return 0;
}

void lsv_ds_free(struct lsv_ds *ds, struct lsv_lmap_cache *map_cache)
{
	BUG_ON(!ds || !map_cache);

	ds->ops->free(ds, map_cache);
}

void *lsv_ds_lookup(struct lsv_ds *ds, sector_t key)
{
	return ds->ops->lookup(ds, key);
}

s32 lsv_ds_insert(struct lsv_ds *ds, sector_t key, void *value, struct lsv_lmap_cache *map_cache)
{
	BUG_ON(!map_cache);

	return ds->ops->insert(ds, key, value, map_cache);
}

void lsv_ds_remove(struct lsv_ds *ds, sector_t key, struct kmem_cache *cell_cachep)
{
	BUG_ON(!cell_cachep);

	ds->ops->remove(ds, key, cell_cachep);
}

sector_t lsv_ds_last(struct lsv_ds *ds, sector_t key)
{
	return ds->ops->last(ds, key);
}

void *lsv_ds_prev(struct lsv_ds *ds, sector_t key, sector_t *prev_key)
{
	BUG_ON(!prev_key);

	return ds->ops->prev(ds, key, prev_key);
}

bool lsv_ds_empty_check(struct lsv_ds *ds)
{
	return ds->ops->empty(ds);
}
