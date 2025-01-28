/* SPDX-License-Identifier: GPL-2.0-only */

#include <linux/hashtable.h>
#include <linux/llist.h>
#pragma once

#define HT_MAP_BITS 7
#define CHUNK_SIZE (1024 * 2)
#define BUCKET_NUM ((sector_t)(key / (CHUNK_SIZE)))

#define MEM_CHECK(ptr)							  \
		if (!node)								  \
			goto mem_err;						  \

#define lhash_for_each_safe(name, bkt, tmp, obj, member)			\
	for ((bkt) = 0, obj = NULL; obj == NULL && (bkt) < HASH_SIZE(name);\
			(bkt)++)\
		llist_for_each_entry_safe(obj, tmp, (struct llist_node *)&name[bkt], member)

#define DECLARE_LHASHTABLE(name, bits)                                   	\
	struct llist_head name[1 << (bits)]

#define lhash_init(hashtable) __lhash_init(hashtable, HASH_SIZE(hashtable))

struct hashtable {
	DECLARE_LHASHTABLE(head, HT_MAP_BITS);
	struct hash_el *last_el;
	u8 nf_bck;
};

struct hash_el {
	struct llist_node node;
	sector_t key;
	void *value;
};

void hash_insert(struct hashtable *hm, struct llist_node *node, sector_t key);
void hashtable_free(struct hashtable *hm);
struct hash_el *hashtable_find_node(struct hashtable *hm, sector_t key);
struct hash_el *hashtable_prev(struct hashtable *hm, sector_t key, sector_t *prev_key);
void hashtable_remove(struct hashtable *hm, sector_t key);
void __lhash_init(struct llist_head *htm, unsigned int size);
