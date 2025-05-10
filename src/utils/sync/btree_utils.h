// SPDX-License-Identifier: GPL-2.0-only

#ifndef BTREE_UTILS_H
#define BTREE_UTILS_H

#include <linux/btree.h>

#define LONG_PER_U64 (64 / BITS_PER_LONG) // irrational, bc driver is suitable only for 64bit systems
#define MAX_KEYLEN (2 * LONG_PER_U64)

struct btree {
	struct btree_head *head;
};

struct btree_geo {
	s32 keylen; // Length of a key in units of unsigned long.
	s32 no_pairs; // Number of key slots (and thus potential value/child pointers) in a node.
	s32 no_longs; // Total number of unsigned longs occupied by all keys in a node. (keylen * no_pairs)
};

/**
 * Retrieves the smallest key present in the B-tree.
 *
 * This function traverses to the leftmost leaf of the B-tree and returns
 * the first key stored in that leaf node.
 * Note: The 'key' parameter is unused in the current implementation.
 *
 * @param head - Pointer to the B-tree head structure.
 * @param geo - Pointer to the B-tree geometry structure.
 * @param key - Unused parameter (potentially intended for output, but not used).
 *
 * @return The smallest key (sector_t) in the B-tree, or 0 if the tree is empty.
 */
sector_t btree_last_no_rep(struct btree_head *head, struct btree_geo *geo, unsigned long *key);

/**
 * Finds the key-value pair immediately succeeding the given key in the B-tree.
 *
 * The search effectively starts by looking for an element greater than 'key - 1'.
 * If a successor is found, its key is copied into the 'key' parameter,
 * and a pointer to its value is returned.
 *
 * @param head - Pointer to the B-tree head structure.
 * @param geo - Pointer to the B-tree geometry structure.
 * @param key - Input: Pointer to the key from which to find the next element.
 *              Output: If a successor is found, this is updated to the successor's key.
 *
 * @return Pointer to the value associated with the successor key, or NULL if
 * no successor is found (e.g., input key is the largest, tree is empty,
 * key is zero, or an internal search miss occurs).
 */
void *btree_get_next(struct btree_head *head, struct btree_geo *geo, unsigned long *key);

/**
 * Finds the key-value pair immediately preceding the given key in the B-tree.
 *
 * If a predecessor is found, its key is stored in the location pointed to by 'prev_key',
 * and a pointer to its value is returned.
 *
 * @param head - Pointer to the B-tree head structure.
 * @param geo - Pointer to the B-tree geometry structure.
 * @param key - Input: Pointer to the key from which to find the previous element.
 * @param prev_key - Output: If a predecessor is found, its key is stored here.
 *                   The caller should ensure this points to valid memory.
 *                   If no predecessor is found, its content is unreliable.
 *
 * @return Pointer to the value associated with the predecessor key, or NULL if
 * no predecessor is found (e.g., input key is the smallest, tree is empty,
 * key is zero, or an internal search miss occurs).
 */
void *btree_get_prev_no_rep(struct btree_head *head, struct btree_geo *geo, unsigned long *key, unsigned long *prev_key);

#endif
