/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef SKIPLIST_H
#define SKIPLIST_H

#include <linux/module.h>

#define HEAD_KEY ((sector_t)0)
#define HEAD_VALUE NULL
#define MAX_LVL 24

// Node unlink statuses for find_pred
enum unlink { FORCE_UNLINK, ASSIST_UNLINK, DONT_UNLINK };

struct skiplist_node {
	sector_t key;
	void *value;
	u32 height;
	struct skiplist_node *removed_link;
	size_t next[]; // array of markable pointer
};

struct skiplist {
	struct skiplist_node *head;
	atomic64_t max_lvl; // max historic number of levels
	sector_t last_key;
	atomic64_t removed_stack_head;
};

/**
 * Simply initialises the skiplist.
 * Allocates it using cache (lsbdd_node_cache).
 * Adds two-side guards in skiplist.
 *
 * @param lsbdd_node_cache - lsbdd_node_cache
 *
 * @return skiplist general structure
 */
struct skiplist *skiplist_init(struct kmem_cache *lsbdd_node_cache);

/**
 * Searches for node with similar key in provided skiplist.
 * !Note: uses find_preds underneath, does not unlink the nodes.
 *
 * @param sl - skiplist to search in
 * @param key - LBA sector
 *
 * @return node on success, NULL on fail
 */
struct skiplist_node *skiplist_find_node(struct skiplist *sl, sector_t key);

/**
 * Frees the allocated resources of skiplist.
 * Deallocates the cache mem by:
 * - iterating through the general skiplist structure
 * - iterating through the removed stack
 * - freeing the guards
 *
 * @param sl - skiplist structure
 * @param lsbdd_node_cache
 * @param lsbdd_value_cache
 *
 * @return void
 */
void skiplist_free(struct skiplist *sl, struct kmem_cache *lsbdd_node_cache, struct kmem_cache *lsbdd_value_cache);

/**
 * Inserts node with key-value data into the skiplist.
 *
 * @param sl - skiplist structure
 * @param key - LBA sector
 * @param value - pointer to struct (value_redir) with PBA and meta data
 * @param lsbdd_node_cache
 * @param lsbdd_value_cache
 *
 * @return inserted node on success, NULL on fail
 */
struct skiplist_node *skiplist_insert(struct skiplist *sl, sector_t key, void *value, struct kmem_cache *lsbdd_node_cache, struct kmem_cache *lsbdd_value_cache);

/**
 * Logically removes the node from the structure.
 * Memory reclamation (physical remove) is made by addding the node to the removed stack
 * and future skiplist_free call.
 *
 * @param sl - skiplist
 * @param key - LBA sector
 * @param lsbdd_value_cache
 *
 * @return void
 */
void skiplist_remove(struct skiplist *sl, sector_t key, struct kmem_cache *lsbdd_value_cache);

/**
 * Searches for node with maximum key being smaller than provided one.
 *
 * @param sl - skiplist
 * @param key - LBA sector
 * @param prev_key - pointer to prev_key
 *
 * @return node on success (with provided key), NULL on fail
 * !Note: also writes in prev_key the found node's key
 */
struct skiplist_node *skiplist_prev(struct skiplist *sl, sector_t key, sector_t *prev_key);

/**
 * Simply returns the last_key from skiplist.
 * It is stored in the general structure.
 */
sector_t skiplist_last(struct skiplist *sl);

// Checks if the next node from head is NULL
bool skiplist_is_empty(struct skiplist *sl);

#endif
