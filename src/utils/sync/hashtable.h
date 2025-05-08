/* SPDX-License-Identifier: GPL-2.0-only */

#include <linux/hashtable.h>
#include <linux/slab.h>
#pragma once

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

struct hashtable *hashtable_init(struct kmem_cache *ht_cache);
struct hash_el *hashtable_insert(struct hashtable *hm, sector_t key, void *value, struct kmem_cache *ht_cache);
void hashtable_free(struct hashtable *hm, struct kmem_cache *ht_cache);
struct hash_el *hashtable_find_node(struct hashtable *hm, sector_t key);
struct hash_el *hashtable_prev(struct hashtable *hm, sector_t key, sector_t *prev_key);
void hashtable_remove(struct hashtable *hm, sector_t key);
bool hashtable_is_empty(struct hashtable *ht);
