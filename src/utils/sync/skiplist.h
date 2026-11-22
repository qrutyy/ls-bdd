/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Lazy skip list: the blocking counterpart of the lock-free skip list.
 *
 * Herlihy M., Lev Y., Luchangco V., Shavit N. A Simple Optimistic Skiplist
 * Algorithm. SIROCCO 2007.
 *
 * Lookups take no locks. Insertions and removals lock the (few) predecessor
 * nodes, validate the optimistic traversal and only then change the links.
 * Every node carries its own spinlock, so the locks are non-sleeping.
 *
 * Memory reclamation follows the lock-free skip list on purpose, so the two
 * are comparable: an unlinked node is pushed on a stack and freed, together
 * with the value it holds, when the skip list is freed. Lookups take no locks,
 * so a node (or a value) can still be read after it was unlinked.
 */

#ifndef SKIPLIST_H
#define SKIPLIST_H

#include <linux/module.h>
#include <linux/spinlock.h>
#include <linux/atomic.h>

#define MAX_LVL 20

struct skiplist_node {
	sector_t key;
	void *value; /* replaced under |lock|, read without it */
	spinlock_t lock;
	u32 height;
	bool marked; /* logically removed, set under |lock| */
	bool fully_linked; /* linked on every level, i.e. visible */
	struct skiplist_node *removed_link; /* stack of unlinked nodes */
	struct skiplist_node *next[]; /* |height| of them are used */
};

/* A value an insertion over an existing key took out of its node. */
struct skiplist_old_value {
	void *value;
	struct skiplist_old_value *next;
};

struct skiplist {
	struct skiplist_node *head; /* sentinels: compared by address, not by key, */
	struct skiplist_node *tail; /* so every sector number is a valid key */
	atomic_t max_lvl; /* historic number of levels, never decreases */
	atomic64_t last_key; /* greatest key ever inserted */
	struct skiplist_node *removed_head;
	struct skiplist_old_value *old_values;
};

/**
 * Allocates the skip list and its two sentinels from |lsbdd_node_cache|.
 * The cache objects must be able to hold a node of MAX_LVL levels.
 *
 * @return skiplist on success, NULL on memory error
 */
struct skiplist *skiplist_init(struct kmem_cache *lsbdd_node_cache);

/**
 * Finds the node with the given key. Takes no locks.
 *
 * @return the node, NULL if there is none (or it is being inserted/removed)
 */
struct skiplist_node *skiplist_find_node(struct skiplist *sl, sector_t key);

/**
 * Frees the skip list: linked nodes, unlinked ones, and all their values.
 * Does not destroy |lsbdd_node_cache|, its owner does. Must not run
 * concurrently with any other operation.
 */
void skiplist_free(struct skiplist *sl, struct kmem_cache *lsbdd_node_cache, struct kmem_cache *lsbdd_value_cache);

/* Prints the bottom level, for debugging. */
void skiplist_print(struct skiplist *sl);

/**
 * Inserts the key-value pair. If the key is present, its value is replaced and
 * the old one is kept until skiplist_free().
 *
 * @param value - pointer to struct (lsbdd_value_redir) with PBA and meta data
 *
 * @return the node on success, NULL on memory error (the value is not taken)
 */
struct skiplist_node *skiplist_insert(struct skiplist *sl, sector_t key, void *value, struct kmem_cache *lsbdd_node_cache,
				      struct kmem_cache *lsbdd_value_cache);

/**
 * Removes the node with the given key, if any. Neither the node nor its value
 * is freed here (see the top of this file).
 */
void skiplist_remove(struct skiplist *sl, sector_t key, struct kmem_cache *lsbdd_value_cache);

/**
 * Finds the node with the greatest key strictly smaller than |key|.
 *
 * @param prev_key - receives the key of the node found
 *
 * @return the node, NULL if every key is >= |key|
 */
struct skiplist_node *skiplist_prev(struct skiplist *sl, sector_t key, sector_t *prev_key);

/* The greatest key ever inserted, 0 if there was none. Never decreases. */
sector_t skiplist_last(struct skiplist *sl);

bool skiplist_is_empty(struct skiplist *sl);

#endif
