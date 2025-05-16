// SPDX-License-Identifier: GPL-2.0-only

#include <linux/hashtable.h>
#include "hashtable.h"
#include <linux/slab.h>
#include "lf_list.h"
#include "atomic_ops.h"
#include <linux/math.h>

/**
 * Initialises all the buckets in hastable.
 *
 * @param ht - pointer to general ht structure
 * @param lsbdd_node_cache - node cache (see list_init in lf_list.c/h)
 *
 * @return void
 */
static bool hash_ll_init(struct hashtable *ht, struct kmem_cache *lsbdd_node_cache) {
	BUG_ON(!ht || !lsbdd_node_cache);
	size_t i = 0;

	for (i = 0; i < BUCKET_COUNT; i++) {
        ht->head[i] = lf_list_init(lsbdd_node_cache);
        if (!ht->head[i]) {
            pr_err("Failed to create list for bucket %lu\n", i);
            kfree(ht->last_el);
			kfree(ht);
            return false;
        }
    }
	return true;
}

struct hashtable *hashtable_init(struct kmem_cache *lsbdd_node_cache) {
	BUG_ON(!lsbdd_node_cache);
    struct hashtable *hash_table = NULL;

    hash_table = kzalloc(sizeof(struct hashtable), GFP_KERNEL);
    if (!hash_table)
        return NULL;

    hash_table->last_el = NULL;
    hash_table->max_bck_num = 0;

	if (!hash_ll_init(hash_table, lsbdd_node_cache)) {
         pr_err("Hashtable: Failed to initialize buckets.\n");
         kfree(hash_table);
         return NULL;
    }
    pr_info("LockFree Hashtable backend initialized with %d buckets.\n", BUCKET_COUNT);

    return hash_table;
}

struct lf_list_node *hashtable_insert(struct hashtable *ht, sector_t key, void *value, struct kmem_cache *lsbdd_node_cache, struct kmem_cache *lsbdd_value_cache)
{

	BUG_ON(!ht || !value || !lsbdd_node_cache);
	struct lf_list_node *el = NULL;

	if (!key) {
		return NULL;
	}

	el = lf_list_add(ht->head[hash_min(BUCKET_NUM, HT_MAP_BITS)], key, value, lsbdd_node_cache);
	if (!el) {
		kmem_cache_free(lsbdd_value_cache, value);
		pr_warn("Hashtable: failed to insert key %llu\n", key);
		return NULL;
	}
	/** Note, that there is a lot of buckets (1 * 2 ** 7) ->
	  * probably few first tens of inserts will be in empty buckets.
	  */

	pr_debug("Hashtable: key %lld written\n", key);
	ht->max_bck_num = max(ht->max_bck_num, BUCKET_NUM);
	if (!ht->last_el) {
		ht->last_el = el;
	} else if (ht->last_el->key < key) {
		ht->last_el = el;
	}

	return el;
}

void hashtable_free(struct hashtable *ht, struct kmem_cache *lsbdd_node_cache, struct kmem_cache *lsbdd_value_cache)
{
	BUG_ON(!ht || !lsbdd_value_cache || !lsbdd_node_cache);
    size_t i = 0;
    if (unlikely(!ht)) return;

    pr_info("Freeing Unsafe Hashtable...\n");
    for (i = 0; i < BUCKET_COUNT; i++) {
        if (ht->head[i]) {
			pr_debug("Hashtable: Cleaning bucket %ld...\n", i);
			lf_list_free(ht->head[i], lsbdd_node_cache, lsbdd_value_cache);
            ht->head[i] = NULL;
        }
    }

    kfree(ht);
    pr_info("Hashtable freed.\n");
}

struct lf_list_node *hashtable_find_node(struct hashtable *ht, sector_t key)
{
    BUG_ON(!ht);
	struct lf_list *list = NULL;
    struct lf_list_node *node = NULL;
	struct lf_list_node *left = NULL;

	list = ht->head[hash_min(BUCKET_NUM, HT_MAP_BITS)];
    if (unlikely(!list)) return NULL;

    node = lf_list_lookup(list, key, &left);

    if (node && node->key == key) {
        pr_debug("Hashtable: Found key %lld (returning casted internal node %p)\n", key, node);
        return node;

    } else if (unlikely(node)) {
		pr_debug("Found node? with key %lld, but searched for %lld\n", node->key, key);
		return NULL;
	} else {
        pr_debug("Hashtable: Key %lld not found\n", key);
        return NULL;
    }
}

struct lf_list_node *hashtable_prev(struct hashtable *ht, sector_t key, sector_t *prev_key)
{
	BUG_ON(!ht);
	struct lf_list_node *node = NULL, *left_node = NULL;
	struct lf_list *list = NULL;

	list = ht->head[hash_min(BUCKET_NUM, HT_MAP_BITS)];

	node = lf_list_lookup(list, key, &left_node);
	if (!node || !left_node || !left_node->key) {
		list = ht->head[hash_min(min(BUCKET_NUM - 1, ht->max_bck_num), HT_MAP_BITS)];
		node = lf_list_lookup(list, key, &left_node);

		pr_info("Found node in prev bucket: %p, key = %llu\n", node, node->key);
		if (!left_node)
			return NULL;
	}

	pr_debug("Hashtable: Element (%p) with prev key - el key=%llu (%llu), val=%p\n", left_node, left_node->key, key, left_node->value);

	*prev_key = left_node->key;

	return left_node;
}

void hashtable_remove(struct hashtable *ht, sector_t key, struct kmem_cache *lsbdd_value_cache)
{
	BUG_ON(!ht);
    struct lf_list *list = NULL;
    bool removed = false;

	list = ht->head[hash_min(BUCKET_NUM, HT_MAP_BITS)];
    if (unlikely(!list)) return;

    removed = lf_list_remove(list, key);

    if (!removed) {
        pr_warn("Hashtable: Tried to remove non-existent key %lld\n", key);
    } else {
         pr_debug("Hashtable: Removed key %lld\n", key);
         // To update the last_el...?
    }
}

bool hashtable_is_empty(struct hashtable *ht) {
    if (!ht)
		return true;

	size_t i = 0;

    for (i = 0; i < BUCKET_COUNT; i++) {
        if (ht->head[i] && ATOMIC_LREAD(&ht->head[i]->size) > 0) {
            return false;
        }
    }
    return true;
}
