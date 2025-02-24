// SPDX-License-Identifier: GPL-2.0-only

#include <linux/hashtable.h>
#include "hashtable-utils.h"
#include <linux/slab.h>



void hash_insert(struct hashtable *ht, struct hlist_node *node, sector_t key)
{
	hlist_add_head(node, &ht->head[hash_min(BUCKET_NUM, HT_MAP_BITS)]);
	ht->nf_bck = BUCKET_NUM;
}

void hashtable_free(struct hashtable *ht)
{
	s32 bckt_iter = 0;
	struct hash_el *el;
	struct hlist_node *tmp;

	hash_for_each_safe(ht->head, bckt_iter, tmp, el, node) {
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
	struct hash_el *el;

	pr_debug("Hashtable: bucket_val %llu", BUCKET_NUM);

	hlist_for_each_entry(el, &ht->head[hash_min(BUCKET_NUM, HT_MAP_BITS)], node)
		if (el != NULL && el->key == key)
			return el;

	return NULL;
}

struct hash_el *hashtable_prev(struct hashtable *ht, sector_t key, sector_t *prev_key)
{
	struct hash_el *prev_max_node = kzalloc(sizeof(struct hash_el), GFP_KERNEL);
	struct hash_el *el;

	hlist_for_each_entry(el, &ht->head[hash_min(BUCKET_NUM, HT_MAP_BITS)], node) {
		if (el && el->key <= key && el->key > prev_max_node->key)
			prev_max_node = el;
	}

	if (prev_max_node->key == 0) {
		pr_debug("Hashtable: Element with  is in the prev bucket\n");
		// mb execute recursively key + mb_size
		hlist_for_each_entry(el, &ht->head[hash_min(min(BUCKET_NUM - 1, ht->nf_bck), HT_MAP_BITS)], node) {
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

void hashtable_remove(struct hashtable *ht, sector_t key)
{
	struct hlist_node *ht_node = NULL;

	ht_node = &hashtable_find_node(ht, key)->node;
	hash_del(ht_node);
}

