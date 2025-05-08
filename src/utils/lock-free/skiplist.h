/* SPDX-License-Identifier: GPL-2.0-only */

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

struct skiplist *skiplist_init(struct kmem_cache *sl_cache);
struct skiplist_node *skiplist_find_node(struct skiplist *sl, sector_t key);
void skiplist_free(struct skiplist *sl, struct kmem_cache *sl_cache, struct kmem_cache *lsbdd_value_cache);
struct skiplist_node *skiplist_insert(struct skiplist *sl, sector_t key, void *data, struct kmem_cache *sl_cache, struct kmem_cache *lsbdd_value_cache);
void skiplist_remove(struct skiplist *sl, sector_t key, struct kmem_cache *lsbdd_value_cache);
struct skiplist_node *skiplist_prev(struct skiplist *sl, sector_t key, sector_t *prev_key);
sector_t skiplist_last(struct skiplist *sl);
bool skiplist_is_empty(struct skiplist *sl);
