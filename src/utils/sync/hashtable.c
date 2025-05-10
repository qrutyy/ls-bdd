// SPDX-License-Identifier: GPL-2.0-only

#include <linux/hashtable.h>
#include "hashtable.h"
#include <linux/slab.h>

struct hashtable *hashtable_init(struct kmem_cache *lsbdd_node_cache)
{
	BUG_ON(!lsbdd_node_cache);

	struct hashtable *hash_table = NULL;
	struct hash_el *last_hel = NULL;

	hash_table = kzalloc(sizeof(struct hashtable), GFP_KERNEL);
	last_hel = kzalloc(sizeof(struct hash_el), GFP_KERNEL);
	hash_table->last_el = last_hel;
	if (!hash_table)
		return NULL;

	hash_init(hash_table->head);
	return hash_table;
}

struct hash_el *hashtable_insert(struct hashtable *ht, sector_t key, void *value, struct kmem_cache *lsbdd_node_cache, struct kmem_cache *lsbdd_value_cache)
{
	BUG_ON(!ht || !lsbdd_node_cache || !lsbdd_value_cache);

	struct hash_el *el = NULL;

	el = kzalloc(sizeof(struct hash_el), GFP_KERNEL); // #TODO fix mem error, handle outside.
	if (!el) {
		pr_err("Hashtable: mem err\n");
		return NULL;
	}

	el->key = key;
	el->value = value;

	hlist_add_head(&el->node, &ht->head[hash_min(BUCKET_NUM, HT_MAP_BITS)]);
	
	ht->max_bck_num = BUCKET_NUM;
	if (ht->last_el->key < key)
		ht->last_el = el;
	return el;
}

void hashtable_free(struct hashtable *ht, struct kmem_cache *lsbdd_node_cache, struct kmem_cache *lsbdd_value_cache)
{
	// TODO: fix deallocation
	BUG_ON(!ht || !lsbdd_node_cache || !lsbdd_value_cache);

	s32 bckt_iter = 0;
	struct hash_el *el = NULL;
	struct hlist_node *tmp = NULL;

	hash_for_each_safe (ht->head, bckt_iter, tmp, el, node) {
		if (el) {
			hash_del(&el->node);
			kfree(el);
		}
	}
	kfree(ht->last_el);
	kfree(ht);
}

struct hash_el *hashtable_find_node(struct hashtable *ht, sector_t key)
{
	BUG_ON(!ht);

	struct hash_el *el = NULL;

	pr_debug("Hashtable: bucket_val %llu", BUCKET_NUM);

	hlist_for_each_entry (el, &ht->head[hash_min(BUCKET_NUM, HT_MAP_BITS)], node)
		if (el != NULL && el->key == key)
			return el;

	return NULL;
}

struct hash_el *hashtable_prev(struct hashtable *ht, sector_t key, sector_t *prev_key)
{
	BUG_ON(!ht);

	struct hash_el *prev_max_node = kzalloc(sizeof(struct hash_el), GFP_KERNEL);
	struct hash_el *el = NULL;

	hlist_for_each_entry (el, &ht->head[hash_min(BUCKET_NUM, HT_MAP_BITS)], node) {
		if (el && el->key <= key && el->key > prev_max_node->key)
			prev_max_node = el;
	}

	if (prev_max_node->key == 0) {
		pr_debug("Hashtable: Element with  is in the prev bucket\n");
		// mb execute recursively key + mb_size
		hlist_for_each_entry (el, &ht->head[hash_min(min(BUCKET_NUM - 1, ht->max_bck_num), HT_MAP_BITS)], node) {
			if (el && el->key <= key && el->key > prev_max_node->key)
				prev_max_node = el;
			pr_debug("Hashtable: prev el key = %llu\n", el->key);
		}
		if (prev_max_node->key == 0)
			return NULL;
	}
	pr_debug("Hashtable: Element with prev key - el key=%llu, val=%p\n", prev_max_node->key, prev_max_node->value);

	*prev_key = prev_max_node->key;
	return prev_max_node;
}

void hashtable_remove(struct hashtable *ht, sector_t key, struct kmem_cache *lsbdd_value_cache)
{
	BUG_ON(!ht || !lsbdd_value_cache);

	struct hlist_node *ht_node = NULL;

	ht_node = &hashtable_find_node(ht, key)->node;
	hash_del(ht_node);
}

bool hashtable_is_empty(struct hashtable *ht)
{
	BUG_ON(!ht);
	return hash_empty(ht->head);
}
