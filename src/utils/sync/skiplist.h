/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Originail author: Daniel Vlasenco @spisladqo
 *
 * Modified by Mikhail Gavrilenko on (30.10.24 - last_change)
 * Changes:
 * - add skiplist_prev, skiplist_last
 * - edit input types
 */

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

struct skiplist *skiplist_init(void);
struct skiplist_node *skiplist_find_node(struct skiplist *sl, sector_t key);
void skiplist_free(struct skiplist *sl);
void skiplist_print(struct skiplist *sl);
struct skiplist_node *skiplist_add(struct skiplist *sl, sector_t key, void *data);
void skiplist_remove(struct skiplist *sl, sector_t key);
struct skiplist_node *skiplist_prev(struct skiplist *sl, sector_t key, sector_t *prev_key);
struct skiplist_node *skiplist_last(struct skiplist *sl);
