// SPDX-License-Identifier: GPL-2.0-only

/*
 * Hash table with a lock per bucket, see hashtable.h.
 *
 * Locking rules:
 * - a bucket's lock protects the links of its list against other writers;
 * - lookups take no lock: a node is initialised before it is made reachable
 *   (smp_store_release), and an unlinked node keeps its |next|, so a reader
 *   standing on it walks on to the live part of the list;
 * - nothing is allocated under a lock.
 */

#include "hashtable.h"
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/lockdep.h>

static struct ht_bucket *bucket_of(struct hashtable *ht, sector_t key)
{
	return &ht->buckets[hash_min(key, HT_MAP_BITS)];
}

static void update_last_key(struct hashtable *ht, sector_t key)
{
	s64 cur = atomic64_read(&ht->last_key);

	while ((u64)cur < key && !atomic64_try_cmpxchg(&ht->last_key, &cur, key))
		;
}

static void push_removed(struct hashtable *ht, struct hash_el *el)
{
	struct hash_el *old = READ_ONCE(ht->removed_head);

	do {
		WRITE_ONCE(el->removed_link, old);
	} while (!try_cmpxchg(&ht->removed_head, &old, el));
}

struct hashtable *hashtable_init(struct kmem_cache *lsbdd_node_cache)
{
	struct hashtable *ht = NULL;
	size_t i;

	BUG_ON(!lsbdd_node_cache);

	ht = kzalloc(sizeof(struct hashtable), GFP_KERNEL);
	if (!ht)
		return NULL;

	ht->buckets = kvcalloc(HT_BUCKETS, sizeof(struct ht_bucket), GFP_KERNEL);
	if (!ht->buckets) {
		kfree(ht);
		return NULL;
	}

	for (i = 0; i < HT_BUCKETS; ++i) {
		spin_lock_init(&ht->buckets[i].lock);
		lockdep_set_novalidate_class(&ht->buckets[i].lock);
	}
	atomic64_set(&ht->last_key, 0);
	atomic_set(&ht->used, 0);

	pr_info("Blocking hashtable (lock per bucket) initialized with %lu buckets.\n", HT_BUCKETS);
	return ht;
}

struct hash_el *hashtable_insert(struct hashtable *ht, sector_t key, void *value, struct kmem_cache *lsbdd_node_cache,
				 struct kmem_cache *lsbdd_value_cache)
{
	struct ht_bucket *bucket = NULL;
	struct hash_el *el = NULL;
	struct hash_el *pred = NULL;
	struct hash_el *curr = NULL;
	struct hash_el *old = NULL;

	BUG_ON(!ht || !lsbdd_node_cache);
	if (WARN_ON(!value))
		return NULL;

	el = kmem_cache_zalloc(lsbdd_node_cache, GFP_KERNEL);
	if (!el)
		return NULL;
	el->key = key;
	el->value = value;

	bucket = bucket_of(ht, key);
	spin_lock(&bucket->lock);

	for (curr = bucket->first; curr && curr->key < key; curr = curr->next)
		pred = curr;

	if (curr && curr->key == key) {
		/* A reader on the old node must see a whole list, so swap the nodes. */
		old = curr;
		el->next = old->next;
	} else {
		el->next = curr;
	}

	if (pred)
		smp_store_release(&pred->next, el);
	else
		smp_store_release(&bucket->first, el);

	spin_unlock(&bucket->lock);

	if (old)
		push_removed(ht, old);
	update_last_key(ht, key);
	if (!atomic_read(&ht->used))
		atomic_set(&ht->used, 1);

	return el;
}

struct hash_el *hashtable_find_node(struct hashtable *ht, sector_t key)
{
	struct hash_el *el = NULL;

	BUG_ON(!ht);

	for (el = READ_ONCE(bucket_of(ht, key)->first); el && el->key < key; el = READ_ONCE(el->next))
		;

	return (el && el->key == key) ? el : NULL;
}

/* The node with the greatest key smaller than |key| in the bucket, NULL if none. */
static struct hash_el *smaller_in_bucket(struct ht_bucket *bucket, sector_t key)
{
	struct hash_el *pred = NULL;
	struct hash_el *el = NULL;

	for (el = READ_ONCE(bucket->first); el && el->key < key; el = READ_ONCE(el->next))
		pred = el;

	return pred;
}

struct hash_el *hashtable_prev(struct hashtable *ht, sector_t key, sector_t *prev_key)
{
	u32 bucket_num = hash_min(key, HT_MAP_BITS);
	struct hash_el *el = NULL;

	BUG_ON(!ht);

	el = smaller_in_bucket(&ht->buckets[bucket_num], key);
	if (!el && bucket_num > 0)
		el = smaller_in_bucket(&ht->buckets[bucket_num - 1], key);
	if (!el)
		return NULL;

	if (prev_key)
		*prev_key = el->key;
	return el;
}

void hashtable_remove(struct hashtable *ht, sector_t key, struct kmem_cache *lsbdd_value_cache)
{
	struct ht_bucket *bucket = NULL;
	struct hash_el *pred = NULL;
	struct hash_el *curr = NULL;

	BUG_ON(!ht);

	bucket = bucket_of(ht, key);
	spin_lock(&bucket->lock);

	for (curr = bucket->first; curr && curr->key < key; curr = curr->next)
		pred = curr;

	if (!curr || curr->key != key) {
		spin_unlock(&bucket->lock);
		return;
	}

	if (pred)
		smp_store_release(&pred->next, curr->next);
	else
		smp_store_release(&bucket->first, curr->next);

	spin_unlock(&bucket->lock);

	push_removed(ht, curr);
}

sector_t hashtable_last(struct hashtable *ht)
{
	BUG_ON(!ht);
	return atomic64_read(&ht->last_key);
}

bool hashtable_is_empty(struct hashtable *ht)
{
	if (!ht)
		return true;
	return !atomic_read(&ht->used);
}

void hashtable_free(struct hashtable *ht, struct kmem_cache *lsbdd_node_cache, struct kmem_cache *lsbdd_value_cache)
{
	struct hash_el *el = NULL;
	struct hash_el *next = NULL;
	size_t i;

	BUG_ON(!ht || !lsbdd_node_cache || !lsbdd_value_cache);

	pr_info("Freeing blocking hashtable...\n");

	for (i = 0; i < HT_BUCKETS; ++i) {
		for (el = ht->buckets[i].first; el; el = next) {
			next = el->next;
			if (el->value)
				kmem_cache_free(lsbdd_value_cache, el->value);
			kmem_cache_free(lsbdd_node_cache, el);
		}
	}

	for (el = ht->removed_head; el; el = next) {
		next = el->removed_link;
		if (el->value)
			kmem_cache_free(lsbdd_value_cache, el->value);
		kmem_cache_free(lsbdd_node_cache, el);
	}

	kvfree(ht->buckets);
	kfree(ht);
	pr_info("Hashtable freed.\n");
}
