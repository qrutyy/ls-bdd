/* SPDX-License-Identifier: GPL-2.0-only */

#include <linux/module.h>
#include <linux/list.h>

#define HEAD_KEY ((sector_t)0)
#define HEAD_VALUE NULL
#define MAX_LVL 24

// SYNC GCC SPECIFIC
// Can be set as gcc function __sync_val_compare_and_swap, both of them support atomicity so basically it doesn't matter (atomic-gcc.h even has a macro that makes it equal)

#define SYNC_CAS(ptr, old, new) cmpxchg(ptr, old, (__typeof__(*ptr))new)
#define SYNC_INC(ptr) atomic_inc(ptr)
#define SYNC_SWAP(ptr, val) xchg(ptr, val)

// TAGS SPECIFIC

#define TAG_VALUE(v, tag) ((v) | tag)
#define IS_TAGGED(v, tag) ((v) & tag)
#define STRIP_TAG(v, tag) ((v) & ~tag)

// marks the node by switching the last bit to 1 and vice versa
#define MARK_NODE(x) TAG_VALUE((size_t)(x), 0x1)
#define HAS_MARK(x) (IS_TAGGED((x), 0x1) == 0x1)
#define GET_NODE(x) ((struct skiplist_node *)(x))
// cleans the pointer from the mark
#define STRIP_MARK(x) ((struct skiplist_node *)STRIP_TAG((x), 0x1))

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
	atomic_t max_lvl; // max historic number of levels
	sector_t last_key;
	atomic_t mr_count;
	atomic_long_t removed_stack_head;
};

struct skiplist *skiplist_init(struct kmem_cache *sl_cache);
struct skiplist_node *skiplist_find_node(struct skiplist *sl, sector_t key);
void skiplist_free(struct skiplist *sl, struct kmem_cache *sl_cache, struct kmem_cache *lsbdd_value_cache);
struct skiplist_node *skiplist_insert(struct skiplist *sl, sector_t key, void *data, struct kmem_cache *sl_cache);
void skiplist_remove(struct skiplist *sl, sector_t key, struct kmem_cache *lsbdd_value_cache);
struct skiplist_node *skiplist_prev(struct skiplist *sl, sector_t key, sector_t *prev_key);
sector_t skiplist_last(struct skiplist *sl);
bool skiplist_is_empty(struct skiplist *sl);
