/* SPDX-License-Identifier: GPL-2.0-only */

#include <linux/rbtree.h>
#include <linux/types.h>

#pragma once

struct rbtree_node {
	struct rb_node node;
	sector_t key;
	void *value;
};

struct rbtree {
	struct rb_root root;
	u64 node_num;
	//struct rbtree_node *last_el; can be added in future
};

struct rbtree *rbtree_init(void);
void rbtree_free(struct rbtree *rbt);
void rbtree_add(struct rbtree *rbt, sector_t key, void *value);
void rbtree_remove(struct rbtree *rbt, sector_t key);
struct rbtree_node *rbtree_find_node(struct rbtree *rbt, sector_t key);
struct rbtree_node *rbtree_prev(struct rbtree *rbt, sector_t key, sector_t *prev_key);
struct rbtree_node *rbtree_last(struct rbtree *rbt);
