/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef LF_LIST_H
#define LF_LIST_H

#include "marked_pointers.h"

struct lf_list_node {
	struct lf_list_node *next;
	struct lf_list_node *removed_link;
	void *value;
	sector_t key;
};

struct lf_list {
	struct lf_list_node *head, *tail;
	atomic64_t removed_stack_head;
	atomic64_t size;
};

/**
 * Initialises list structure. Adds list guards (new border nodes).
 *
 * @param list_node_cache - node cache ;)
 *
 * @return lf_list pointer on success, NULL on error
 */
struct lf_list *lf_list_init(struct kmem_cache *lsbdd_node_cache);
/**
 * Frees the memory allocated for linked list.
 *
 * @param list - pointer to general list structure
 * @param lsbdd_node_cache - node cache
 * @param lsbdd_value_cache - value (redir) cache
 *
 * @return void
 */
void lf_list_free(struct lf_list *list, struct kmem_cache *lsbdd_node_cache, struct kmem_cache *lsbdd_value_cache);

/**
 * Adds element to the list in sorted (ascending) order.
 *
 * @param list - pointer to general list structure
 * @param key - LBA sector_t
 * @param value - pointer to struct (lsbdd_value_redir) with PBA and meta data
 * @param list_node_cache - node cache ;)
 *
 * @return pointer to inserted node on success, NULL on error
 */
struct lf_list_node *lf_list_add(struct lf_list *list, sector_t key, void *val, struct kmem_cache *lf_list_node_cache);

/* The deletion is logical and consists of setting the node mark bit to 1.
 * After logically deleting the node - it is added into removed_stack for future memory reclamation.
 * Physical deletion (memory reclamation) is handled in list_free.
 *
 * @param list - pointer to general list structure
 * @param key - LBA sector_t
 *
 * @return true on success (if found and removed), false on error
 */
bool lf_list_remove(struct lf_list *list, sector_t key);

/* Looks for value val, it
 *  - returns right_node owning val (if present) or its immediately higher
 *    value present in the list (otherwise) and
 *  - sets the left_node to the node owning the value immediately lower than
 *    val.
 *  - returns NULL if:
 *    - key 0 is searched (bug occures when key 0 is searched and its too hard to fix, but we've set the offset, so it shouldn't issue the fio)
 *    - MAX_RETRIES limit was passed
 *    - there is infinite loop cause (line 82)
 *
 * Encountered nodes that are marked as logically deleted are physically removed
 * from the list, yet not garbage collected.
 *
 * @param list - pointer to general list structure
 * @param key - LBA sector_t
 * @param left_node - pointer to left_node location
 *
 * @return node on success, NULL on error
 */
struct lf_list_node *lf_list_lookup(struct lf_list *list, sector_t key, struct lf_list_node **left_node);

#endif
