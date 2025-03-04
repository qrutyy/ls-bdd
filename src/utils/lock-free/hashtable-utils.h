/* SPDX-License-Identifier: GPL-2.0-only */

#include <linux/hashtable.h>
#include <linux/llist.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#pragma once

#define HT_MAP_BITS 7
#define CHUNK_SIZE (1024 * 2)
#define BUCKET_NUM ((sector_t)(key / (CHUNK_SIZE)))

#define MEM_CHECK(ptr)													\
		if (!node)														\
			goto mem_err;												\

#define lhash_for_each_safe(name, bkt, tmp, obj, member)				\
	for ((bkt) = 0, obj = NULL; obj == NULL && (bkt) < HASH_SIZE(name);	\
			(bkt)++)													\
		llist_for_each_entry_safe(obj, tmp, (struct llist_node *)&name[bkt].first->next, member)

#define DECLARE_LHASHTABLE(name, bits)                                  \
	struct llist_head name[1 << (bits)]

#define lhash_init(hashtable) __lhash_init(hashtable, HASH_SIZE(hashtable))

struct hashtable {
	DECLARE_LHASHTABLE(head, HT_MAP_BITS);
	struct hash_el *last_el;
	u8 max_bck_num;
	spinlock_t lock[1 << HT_MAP_BITS];
};

struct hash_el {
	struct llist_node node;
	sector_t key;
	void *value;
};

struct hashtable *hashtable_init(struct kmem_cache *ht_cache);
struct hash_el *hashtable_insert(struct hashtable *ht, sector_t key, void* value, struct kmem_cache *ht_cache);
void hashtable_free(struct hashtable *ht, struct kmem_cache *ht_cache);
struct hash_el *hashtable_find_node(struct hashtable *ht, sector_t key);
struct hash_el *hashtable_prev(struct hashtable *ht, sector_t key, sector_t *prev_key);
void hashtable_remove(struct hashtable *ht, sector_t key);
void __lhash_init(struct llist_head *htm, unsigned int size);
bool hashtable_is_empty(struct hashtable *ht);
