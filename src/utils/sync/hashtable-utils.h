/* SPDX-License-Identifier: GPL-2.0-only */

#pragma once

#define HT_MAP_BITS 7
#define CHUNK_SIZE (1024 * 2)
#define BUCKET_NUM ((sector_t)(key / (CHUNK_SIZE)))

struct hashtable {
	DECLARE_HASHTABLE(head, HT_MAP_BITS);
	struct hash_el *last_el;
	u8 nf_bck;
};

struct hash_el {
	sector_t key;
	void *value;
	struct hlist_node node;
};

void hash_insert(struct hashtable *hm, struct hlist_node *node, sector_t key);
void hashtable_free(struct hashtable *hm);
struct hash_el *hashtable_find_node(struct hashtable *hm, sector_t key);
struct hash_el *hashtable_prev(struct hashtable *hm, sector_t key, sector_t *prev_key);
void hashtable_remove(struct hashtable *hm, sector_t key);
