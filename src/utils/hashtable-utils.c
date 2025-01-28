// SPDX-License-Identifier: GPL-2.0-only

#include <linux/hashtable.h>
#include "hashtable-utils.h"
#include <linux/slab.h>
#include <linux/llist.h>
/**
	* SMP modification of basic kernel hashtable using llist.
	*/


void __lhash_init(struct llist_head *ht, unsigned int size)
{
	unsigned int i;

	for (i = 0; i < size; i++)
		init_llist_head(&ht[i]);
}

void hash_insert(struct hashtable *ht, struct llist_node *node, sector_t key)
{
	if (llist_add(node, &ht->head[hash_min(BUCKET_NUM, HT_MAP_BITS)]))
		pr_warn("Hashtable: was empty\n");

	ht->nf_bck = BUCKET_NUM;
}

void hashtable_free(struct hashtable *ht)
{
	s32 bckt_iter = 0;
	struct hash_el *el, *tmp = NULL;

	lhash_for_each_safe(ht->head, bckt_iter, tmp, el, node) {
		if (el) {
			kfree(el);
			xchg(&el, NULL);
		}
	}
	pr_debug("Hashtable: finished clearing the hashtable inside\n");
	kfree(ht);
	pr_debug("2\n");
	xchg(&ht, NULL);
}

struct hash_el *hashtable_find_node(struct hashtable *ht, sector_t key)
{
	struct hash_el *el, *tmp = NULL;

	pr_debug("Hashtable: bucket_val %llu", BUCKET_NUM);

	llist_for_each_entry_safe(el, tmp, ht->head[hash_min(BUCKET_NUM, HT_MAP_BITS)].first, node) {
		if (el != NULL && el->key == key)
			return el;
	}
	return NULL;
}

struct hash_el *hashtable_prev(struct hashtable *ht, sector_t key, sector_t *prev_key)
{
	struct hash_el *prev_max_node = NULL; 
	
	prev_max_node = kzalloc(sizeof(struct hash_el), GFP_KERNEL);
	if (!prev_max_node)
		return NULL;

	struct hash_el *el, *tmp = NULL;

	llist_for_each_entry_safe(el, tmp, ht->head[hash_min(BUCKET_NUM, HT_MAP_BITS)].first, node) {
		if (el && el->key <= key && el->key > prev_max_node->key)
			prev_max_node = el;
	}

	if (prev_max_node->key == 0) {
		pr_debug("Hashtable: Previous element is in the previous bucket\n");
		llist_for_each_entry(el, ht->head[hash_min(min(BUCKET_NUM - 1, ht->nf_bck), HT_MAP_BITS)].first, node) {
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


// spinlock it 
void hashtable_remove(struct hashtable *ht, sector_t key)
{
// todo	
}


// todo
static inline void llist_del_init(struct hlist_node *n)
{
	if (!n->pprev) {
		__hlist_del(n);
		INIT_HLIST_NODE(n);
	}
}


static inline void __llist_del(struct hlist_node *n)
{
	struct hlist_node *next = n->next;
	struct hlist_node **pprev = n->pprev;

	WRITE_ONCE(*pprev, next);
	if (next)
		WRITE_ONCE(next->pprev, pprev);
}

