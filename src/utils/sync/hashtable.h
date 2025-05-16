/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef HASHTABLE_H
#define HASHTABLE_H

#include <linux/hashtable.h>
#include <linux/slab.h>

#define HT_MAP_BITS 7
#define CHUNK_SIZE (1024 * 2)
#define BUCKET_NUM ((sector_t)(key / (CHUNK_SIZE)))

struct hashtable {
	DECLARE_HASHTABLE(head, HT_MAP_BITS);
	struct hash_el *last_el;
	u8 max_bck_num;
};

struct hash_el {
	sector_t key;
	void *value;
	struct hlist_node node;
};

/**
 * Simply initialises the hashtable and allocates guard nodes.
 *
 * @param lsbdd_node_cache - node cache ;)
 *
 * @return hashtable structure, NULL on error
 */
struct hashtable *hashtable_init(struct kmem_cache *lsbdd_node_cache);

/**
 * Inserts node with provided key-value pair into the hashtable.
 *
 * @param ht - hashtable structure
 * @param key - LBA sector
 * @param value - pointer to struct (value_redir) with PBA and meta data
 * @param lsbdd_node_cache
 * @param lsbdd_value_cache
 *
 * @return inserted node, NULL if:
 *  - if key == 0
 *  - if failed to insert key (check lf_list_add)
 */
struct hash_el *hashtable_insert(struct hashtable *hm, sector_t key, void *value, struct kmem_cache *lsbdd_node_cache, struct kmem_cache *lsbdd_value_cache);

/**
 * Frees the allocated memory and caches.
 * Just iterates over the buckets and calls frees all the nodes.
 *
 * @param ht - hastable structure
 * @param lsbdd_node_cache - node cache
 * @param lsbdd_value_cache - value cache
 *
 * @return void
 */
void hashtable_free(struct hashtable *hm, struct kmem_cache *lsbdd_node_cache, struct kmem_cache *lsbdd_value_cache);

/**
 * Searches for node with provided key in the hashtable.
 *
 * @param ht - hastable structure
 * @param key - LBA sector
 *
 * @return node on success, NULL on fail
 */
struct hash_el *hashtable_find_node(struct hashtable *hm, sector_t key);

/**
 * Searches for node with max key smaller than provided one.
 *
 * @param ht - hastable structure
 * @param key - LBA sector
 * @param prev_key - pointer to prev_key memory that will be changed
 *
 * @return prev_node on success, NULL on fail
 */
struct hash_el *hashtable_prev(struct hashtable *hm, sector_t key, sector_t *prev_key);

/**
 * Removes the node from the hashtable. Frees the node specific mem.
 *
 * @param ht - hashtable structure
 * @param key - LBA sector
 * @param lsbdd_value_cache - value cache (value is being freed right at the time node is being removed)
 *
 * @return void
 */
void hashtable_remove(struct hashtable *hm, sector_t key, struct kmem_cache *lsbdd_value_cache);

// @return bool if empty
bool hashtable_is_empty(struct hashtable *ht);

#endif
