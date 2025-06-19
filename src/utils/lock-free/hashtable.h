/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef HASHTABLE_H
#define HASHTABLE_H

#include <linux/hashtable.h>
#include <linux/llist.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include "lf_list.h"

/**
 * SMP modification of basic kernel hashtable using lock-free linked list (not doubly though).
 * Kernel hashtable is based on the doubly linked list, organising it in form of buckets.
 *
 * In this modification - lock-free singly linked list is used as the base structure.
 * Memory reclamation system is tied up to it. It uses an additional stack that collects physically removed nodes and stores them until the hashtable_free is called.
 * NOTE!: doubly linked list potentially could help us get prev elements faster (in case of the same bucket), but such struggle doesn't worth the effort.
 */

#define HT_MAP_BITS 17
#define BUCKET_COUNT (1 << HT_MAP_BITS)

#define MEM_CHECK(ptr)                                                                                                                     \
	do {																																\
		if (!ptr)                                                                                                                         \
			goto mem_err;																												\
	} while (0)																														  \

// basic iterator over the nodes
#define lhash_for_each_safe(name, bkt, tmp, obj, member)                                      \
	for ((bkt) = 0, obj = NULL; obj == NULL && (bkt) < HASH_SIZE(name); (bkt)++)          \
		llist_for_each_entry_safe(obj, tmp, ((struct llist_node *)(&(name)[bkt].first->next)), member)
#define DECLARE_LHASHTABLE(name, bits) struct lf_list *name[1 << (bits)]

#define lhash_init(hashtable) __lhash_init(hashtable, HASH_SIZE(hashtable))

struct hashtable {
	DECLARE_LHASHTABLE(head, HT_MAP_BITS);
	struct lf_list_node *last_el;
	u8 max_bck_num;
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
 * @param value - pointer to struct (lsbdd_value_redir) with PBA and meta data
 * @param lsbdd_node_cache
 * @param lsbdd_value_cache
 *
 * @return inserted node, NULL if:
 *  - if key == 0
 *  - if failed to insert key (check lf_list_add)
 */
struct lf_list_node *hashtable_insert(struct hashtable *ht, sector_t key, void *value, struct kmem_cache *lsbdd_node_cache, struct kmem_cache *lsbdd_value_cache);

/**
 * Frees the allocated memory and caches.
 * Just iterates over the buckets and calls lf_list_free. (check it for possible return cases)
 *
 * @param ht - hastable structure
 * @param lsbdd_node_cache - node cache
 * @param lsbdd_value_cache - value cache
 *
 * @return void
 */
void hashtable_free(struct hashtable *ht, struct kmem_cache *lsbdd_node_cache, struct kmem_cache *lsbdd_value_cache);

/**
 * Searches for node with provided key in the hashtable.
 *
 * @param ht - hastable structure
 * @param key - LBA sector
 *
 * @return node on success, NULL if:
 *  - found node with other key
 *  - if lf_list_lookup failed
 */
struct lf_list_node *hashtable_find_node(struct hashtable *ht, sector_t key);

/**
 * Searches for node with max key smaller than provided one.
 *
 * @param ht - hastable structure
 * @param key - LBA sector
 * @param prev_key - pointer to prev_key memory that will be changed
 *
 * @return prev_node on success, NULL if:
 *  - lf_list_lookup returned NULL
 */
struct lf_list_node *hashtable_prev(struct hashtable *ht, sector_t key, sector_t *prev_key);

/**
 * Logically removes the node from the hashtable.
 * !Note: Physical deletion (memory free) is handled at the end of driver's work - at the hashtable_free.
 *
 * @param ht - hashtable structure
 * @param key - LBA sector
 * @param lsbdd_value_cache - value cache (value is being freed right at the time node is being removed)
 *
 * @return void
 */
void hashtable_remove(struct hashtable *ht, sector_t key, struct kmem_cache *lsbdd_value_cache);

// Hashtable initialisator 2000 mega pro.
void __lhash_init(struct llist_head *htm, unsigned int size);

// @return bool if empty
bool hashtable_is_empty(struct hashtable *ht);

#endif
