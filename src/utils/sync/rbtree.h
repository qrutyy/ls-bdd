/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef RBTREE_H
#define RBTREE_H

// JUST STABS, AS LONG AS NO LOCK-FREE B+TREE IS FOUND

#include <linux/rbtree.h>
#include <linux/types.h>

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

/**
 * Initialises a new tree with RB_ROOT.
 *
 * @param void
 *
 * @return rbtree structure
 */
struct rbtree *rbtree_init(void);

/**
 * Frees all the RB tree structure.
 * Iterates in postorder and deallocates all the nodes and their data.
 *
 * @param rbt - rb tree structure
 *
 * @return void
 */
void rbtree_free(struct rbtree *rbt);

/**
 * Adds key-value pair into rb tree structure.
 * For better description - see __rbtree_underlying_insert.
 *
 * @param key - LBA sector
 * @param value -  pointer to structure (value_redir) with PBA and meta data
 *
 * @return void
 */
void rbtree_add(struct rbtree *rbt, sector_t key, void *value);

/**
 * Removes the node from the rb tree structure.
 *
 * @param rbt - rb tree structure
 * @param key - LBA sector
 *
 * @return void
 * !Note: in case of successfull remove - deallocates the mem.
 */
void rbtree_remove(struct rbtree *rbt, sector_t key);

/**
 * Searches for node in general rb tree structure.
 * For better description - see __rbtree_underlying_search.
 *
 * @param rbt - rb tree structure
 * @param key - LBA sector.
 *
 * @return node on success, NULL on fail
 */
struct rbtree_node *rbtree_find_node(struct rbtree *rbt, sector_t key);

/**
 * Searches for node with greatest key smaller then provided one.
 *
 * @param rbt - rb tree structure
 * @param key - LBA sector
 * @param prev_key - pointer to prev_key mem
 *
 * @return NULL on fail, node on success
 */
struct rbtree_node *rbtree_prev(struct rbtree *rbt, sector_t key, sector_t *prev_key);

/**
 * Simply gets the last node by iterating the right side of the tree.
 * !Note: in case it overloads the general performance
 * - it can be optimised by adding pointer to last el into the rbtree structure.
 *
 * @param rbt - rb tree structure
 *
 * @return NULL on fail, node on success
 */
struct rbtree_node *rbtree_last(struct rbtree *rbt);

#endif
