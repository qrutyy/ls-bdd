/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Hash table with a lock per bucket: the blocking counterpart of the lock-free
 * hash table.
 *
 * The table has the same shape as the lock-free one: HT_BUCKETS buckets chosen
 * with hash_min(key, HT_MAP_BITS), a sorted singly linked list in each. The
 * list of a bucket is changed under that bucket's spinlock, so writers of
 * different buckets do not meet. Lookups take no locks.
 *
 * Memory reclamation follows the lock-free table on purpose, so the two are
 * comparable: an unlinked node is pushed on a stack and freed, together with
 * the value it holds, when the table is freed. Lookups take no locks, so a node
 * (or a value) can still be read after it was unlinked.
 */

#ifndef HASHTABLE_H
#define HASHTABLE_H

#include <linux/hashtable.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/atomic.h>

/* 2^18 buckets, like the lock-free table (tests build with fewer to get long chains) */
#ifndef HT_MAP_BITS
#define HT_MAP_BITS 18
#endif
#define HT_BUCKETS (1UL << HT_MAP_BITS)

struct hash_el {
	sector_t key;
	void *value;
	struct hash_el *next; /* the next greater key of the bucket */
	struct hash_el *removed_link; /* stack of unlinked nodes */
};

struct ht_bucket {
	spinlock_t lock; /* protects the list below against other writers */
	struct hash_el *first;
};

struct hashtable {
	struct ht_bucket *buckets;
	atomic64_t last_key; /* greatest key ever inserted */
	atomic_t used; /* set by the first insertion */
	struct hash_el *removed_head;
};

/**
 * Allocates the table and its buckets.
 *
 * @return hashtable on success, NULL on memory error
 */
struct hashtable *hashtable_init(struct kmem_cache *lsbdd_node_cache);

/**
 * Inserts the key-value pair. If the key is present, its node (and value) is
 * replaced; the old ones are kept until hashtable_free().
 *
 * @param value - pointer to struct (lsbdd_value_redir) with PBA and meta data
 *
 * @return the node on success, NULL on memory error (the value is not taken)
 */
struct hash_el *hashtable_insert(struct hashtable *ht, sector_t key, void *value, struct kmem_cache *lsbdd_node_cache,
				 struct kmem_cache *lsbdd_value_cache);

/**
 * Frees the table: linked nodes, unlinked ones, and all their values.
 * Does not destroy the caches, their owner does. Must not run concurrently
 * with any other operation.
 */
void hashtable_free(struct hashtable *ht, struct kmem_cache *lsbdd_node_cache, struct kmem_cache *lsbdd_value_cache);

/**
 * Finds the node with the given key. Takes no locks.
 *
 * @return the node, NULL if there is none
 */
struct hash_el *hashtable_find_node(struct hashtable *ht, sector_t key);

/**
 * Looks for the node with the greatest key smaller than |key|, like the
 * lock-free table does: only in the bucket of |key| and in the one before it.
 * The buckets are not ordered by key, so this is not an exact predecessor
 * query: it can miss the predecessor, and it can find a node of the bucket
 * before that is not the greatest smaller key.
 *
 * @param prev_key - receives the key of the node found
 *
 * @return the node, NULL if none was found
 */
struct hash_el *hashtable_prev(struct hashtable *ht, sector_t key, sector_t *prev_key);

/**
 * Removes the node with the given key, if any. Neither the node nor its value
 * is freed here (see the top of this file).
 */
void hashtable_remove(struct hashtable *ht, sector_t key, struct kmem_cache *lsbdd_value_cache);

/* The greatest key ever inserted, 0 if there was none. Never decreases. */
sector_t hashtable_last(struct hashtable *ht);

/* True until the first insertion. It does not turn true again after removals. */
bool hashtable_is_empty(struct hashtable *ht);

#endif
