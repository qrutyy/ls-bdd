// SPDX-License-Identifier: GPL-2.0-only

#include <linux/hashtable.h>
#include "hashtable-utils.h"
#include <linux/slab.h>
#include <linux/llist.h>

// SMP modification of basic kernel hashtable using llist (not doubly though).

inline void __lhash_init(struct llist_head *ht, unsigned int size)
{
	unsigned int i;

	for (i = 0; i < size; i++)
		init_llist_head(&ht[i]);
}

bool hashtable_init(struct data_struct *ds, struct kmem_cache *ht_cache) {
	struct hashtable *hash_table = NULL;
	struct hash_el *last_hel = NULL;
	
	hash_table = kzalloc(sizeof(struct hashtable), GFP_KERNEL);
	last_hel = kmem_cache_alloc(ht_cache, GFP_KERNEL);
	hash_table->last_el = last_hel;
	if (!(hash_table && last_hel))
		return false;

	lhash_init(hash_table->head);
	ds->type = HASHTABLE_TYPE;
	ds->structure.map_hash = hash_table;
	ds->structure.map_hash->max_bck_num = 0;
	return true;
}

bool hash_insert(struct hashtable *ht, struct llist_node *node, sector_t key, void* value, struct kmem_cache *ht_cache)
{

	struct hash_el *el = NULL;
	el = kmem_cache_alloc(ht_cache, GFP_KERNEL);
	pr_debug("ds-hash: %p, ds-el: %p\n", ht_cache, el);

	if (!el)
		return false;

	el->key = key;
	el->value = value;
	
	if (llist_add(node, &ht->head[hash_min(BUCKET_NUM, HT_MAP_BITS)]))
		pr_debug("Hashtable: was empty\n");
	
	/** Note, that there is a lot of buckets (1 * 2 ** 7) -> 
	  * probably few first tens of inserts will be in empty buckets 
	  */
	
	pr_debug("Hashtable: key %lld written\n", key);
	ht->max_bck_num = BUCKET_NUM;
	
	if (ds->structure.map_hash->last_el->key < key)
		ds->structure.map_hash->last_el = el;

	return true;
}

void hashtable_free(struct hashtable *ht, struct kmem_cache *ht_cache)
{
	s32 bckt_iter = 0;
	struct hash_el *el, *tmp = NULL;
	pr_debug("hash: %p\n", ht_cache);
	lhash_for_each_safe(ht->head, bckt_iter, tmp, el, node) {
		if (el) {
			pr_debug("el %p\n", el);
			kmem_cache_free(ht_cache, el);
			xchg(&el, NULL);
		}
	}
	kfree(ht);
	xchg(&ht, NULL);
}

struct hash_el *hashtable_find_node(struct hashtable *ht, sector_t key)
{
	struct hash_el *el, *tmp = NULL;

	pr_debug("Hashtable: bucket_val %llu", BUCKET_NUM);

	llist_for_each_entry_safe(el, tmp, ht->head[hash_min(BUCKET_NUM, HT_MAP_BITS)].first, node) {
		if (el != NULL && el->key == key) {
			pr_debug("key %lld, found %lld\n", key, el->key);
			return el;
		}
	}
	return NULL;
}

struct hash_el *hashtable_prev(struct hashtable *ht, sector_t key, sector_t *prev_key, struct lsbdd_nodes_cache *node_cache)
{
	struct hash_el *prev_max_node = NULL; 
	
	prev_max_node = kzalloc(sizeof(struct hash_el), GFP_KERNEL);
	if (!prev_max_node)
		return NULL;

	struct hash_el *el, *tmp = NULL;
	sector_t bucket_num = 0;

	llist_for_each_entry_safe(el, tmp, ht->head[hash_min(BUCKET_NUM, HT_MAP_BITS)].first, node) {
		if (el && el->key <= key && el->key > prev_max_node->key)
			prev_max_node = el;
	}

	if (!prev_max_node->value) { // cant compare key with 0, bc sector 0 can appear in the bio!
		bucket_num = (!BUCKET_NUM) ? 0 : BUCKET_NUM - 1;
		pr_debug("Hashtable: key = %lld Previous element is in the previous bucket %lld, bck %lld\n", key, bucket_num, BUCKET_NUM);

		llist_for_each_entry(el, ht->head[hash_min(min(bucket_num, ht->max_bck_num), HT_MAP_BITS)].first, node) {
			if (el && el->key <= key && el->key >= prev_max_node->key)
				prev_max_node = el;

			pr_debug("Hashtable: prev el key = %llu\n", el->key);
		}
		
	}
	pr_debug("Hashtable: Element with prev key - el key=%llu, val=%p\n", prev_max_node->key, prev_max_node->value);
	
	if (!prev_max_node->value) {
		kfree(prev_max_node);
		return NULL;
	}
	
	*prev_key = prev_max_node->key;
	return prev_max_node;
}

static inline void __llist_del(struct llist_node node, struct llist_node prev, spinlock_t lock)
{
	struct llist_node *next = NULL;

	spin_lock(&lock);

	next = node.next;
	WRITE_ONCE(prev.next, next); // should cause a mem_leak i guess
	WRITE_ONCE(node.next, NULL);

	spin_unlock(&lock);
}

void hashtable_remove(struct hashtable *ht, sector_t key)
{
	struct hash_el *el, *tmp, *prev_el = NULL;
	u32 bckt_num = 0;
	spinlock_t bckt_lock;
	
	bckt_num = hash_min(BUCKET_NUM, HT_MAP_BITS);
	bckt_lock = ht->lock[bckt_num]; // per bucket lock

	pr_debug("Hashtable: bucket_val %llu", BUCKET_NUM);
	 // test only. should be moved further to decrease the crit. section
	// no lock or other sync is needed, due to lock-free llist iter
	llist_for_each_entry_safe(el, tmp, ht->head[bckt_num].first, node) {
		if (el != NULL && el->key == key)
			break;
		prev_el = el;
	}
	if (!el) {
		pr_warn("Hashtable: tried to remove not existing element\n");
		return;
	}

	if (prev_el) {
		__llist_del(el->node, prev_el->node, bckt_lock); 
	} else { 
		llist_del_first(&ht->head[bckt_num]);
	}
}

