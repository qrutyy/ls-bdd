/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Originail author: Daniel Vlasenco @spisladqo
 *
 * Modified by Mikhail Gavrilenko on (11.03.25 - last_change)
 * Changes:
 * - add skiplist_prev, skiplist_last
 * - edit input types
 */

#ifndef SKIPLIST_H
#define SKIPLIST_H

#include <linux/module.h>

#define HEAD_KEY ((sector_t)0)
#define HEAD_VALUE NULL
#define TAIL_KEY ((sector_t)U64_MAX)
#define TAIL_VALUE ((sector_t)0)
#define MAX_LVL 20

struct skiplist_node {
	struct skiplist_node *next;
	struct skiplist_node *lower;
	sector_t key;
	void *value;
};

struct skiplist {
	struct skiplist_node *head;
	s32 head_lvl;
	s32 max_lvl;
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
 * 
 * @param sl - skiplist to search in 
 * @param key - LBA sector 
 *
 * @return node on success, NULL on fail (mem alloc fail)
 */
struct skiplist_node *skiplist_find_node(struct skiplist *sl, sector_t key);

/**
 * Frees the allocated resources of skiplist. 
 * Deallocates the cache mem by iterating through the general skiplist structure
 *
 * @param sl - skiplist structure 
 * @param lsbdd_node_cache
 * @param lsbdd_value_cache
 *
 * @return void
 */
void skiplist_free(struct skiplist *sl, struct kmem_cache *lsbdd_node_cache, struct kmem_cache *lsbdd_value_cache);

// prints the skiplist structure
void skiplist_print(struct skiplist *sl);

/**
 * Inserts node with key-value data into the skiplist.
 *
 * @param sl - skiplist structure
 * @param key - LBA sector
 * @param value - pointer to struct (value_redir) with PBA and meta data
 * @param lsbdd_node_cache
 * @param lsbdd_value_cache (not used)
 *
 * @return inserted node on success, NULL on fail
 */
struct skiplist_node *skiplist_insert(struct skiplist *sl, sector_t key, void *data, struct kmem_cache *lsbdd_node_cache, struct kmem_cache *lsbdd_value_cache);

/**
 * Removes the node from the structure. Frees the allocated mem.
 *
 * @param sl - skiplist 
 * @param key - LBA sector
 *
 * @return void
 */
void skiplist_remove(struct skiplist *sl, sector_t key);

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
