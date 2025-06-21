// SPDX-License-Identifier: GPL-2.0-only

#include <linux/module.h>
#include "lf_list.h"
#include "marked_pointers.h"
#include "atomic_ops.h"
#include <linux/slab.h>

#define GET_NODE(x) ((struct lf_list_node *)(x))
// cleans the pointer from the mark
#define STRIP_MARK(x) ((struct lf_list_node *)STRIP_TAG((x), 0x1))
#define MAX_LOOKUP_RETRIES 10000

/**
 * Adds the node to removed stack by replacing the head with the node.
 * It is used only inside the lf_list_remove function to decrease possible corruption cases.
 * Theorethically - it can be used in lookup, when the physical help/unlink happens, but thats a Jimmy Neutron's task.
 *
 * !Note: uses s64, bc of the atomic64_t removed_stack_head. We provide atomic head update.
 * Its the only one for multiple threads, so it can be accessed from multiple threads at one time.
 *
 * @param list - general list structure
 * @param node - node to removed node
 *
 * @return void
 */
static void add_to_removed_stack(struct lf_list *list, struct lf_list_node *node_to_add)
{
	BUG_ON(!node_to_add || !list);

	s64 old_head_val;
	s64 new_head_ptr_val = (s64)node_to_add;

	pr_debug("%s: Attempting to add node %p (key %llu)\n", __func__, node_to_add, node_to_add->key);

	do {
		old_head_val = ATOMIC_LREAD(&list->removed_stack_head);
		pr_debug("%s: Current removed_head is %p\n", __func__, (void *)old_head_val);

		if ((struct lf_list_node *)old_head_val == node_to_add) {
			pr_warn("%s: node %p is already head of removed_stack. Cycle prevented. NOT ADDING.\n", __func__, node_to_add);
			return;
		}
		node_to_add->removed_link = (struct lf_list_node *)old_head_val;
	} while (ATOMIC_LCAS(&list->removed_stack_head, old_head_val, new_head_ptr_val) != old_head_val);

	pr_debug("%s: Successfully added node %p. New head %p, its next %p\n", __func__, node_to_add, (void *)new_head_ptr_val,
		 node_to_add->removed_link);
}

/**
 * Allocates the node and initialises it.
 * Memory allocation fail is handled in callers.
 *
 * @param key - LBA sector_t
 * @param value - pointer to struct (lsbdd_value_redir) with PBA and meta data
 *
 * @return lf_list_node pointer on success, NULL on error
 */
static struct lf_list_node *node_alloc(sector_t key, void *value, struct lf_list_node *next, struct kmem_cache *node_cache)
{
	struct lf_list_node *node = kmem_cache_zalloc(node_cache, GFP_KERNEL);

	if (!node)
		return NULL;

	node->key = key;
	node->value = value;
	node->next = next;
	node->removed_link = NULL;

	return node;
}

struct lf_list *lf_list_init(struct kmem_cache *list_node_cache)
{
	struct lf_list *list = kzalloc(sizeof(struct lf_list), GFP_KERNEL);

	list->head = node_alloc(U32_MIN, NULL, NULL, list_node_cache);
	list->tail = node_alloc(U32_MAX, NULL, NULL, list_node_cache);
	if (!list->tail || !list->head) {
		kfree(list);
		return NULL;
	}

	list->head->next = list->tail;
	atomic64_set(&list->removed_stack_head, 0);
	atomic64_set(&list->size, 1);

	return list;
}

void lf_list_free(struct lf_list *list, struct kmem_cache *lsbdd_node_cache, struct kmem_cache *lsbdd_value_cache)
{
	BUG_ON(!list || !lsbdd_node_cache || !lsbdd_value_cache);

	struct lf_list_node *node = NULL;
	struct lf_list_node *next = NULL;
	struct lf_list_node *removed_node_head = NULL;
	void *last_freed_node_addr = NULL;

	node = STRIP_MARK(list->head->next);

	pr_debug("%s: Starting main list traversal from node %p\n", __func__, node);
	while (node && node != list->tail) {
		next = STRIP_MARK(node->next);

		if (node == last_freed_node_addr) { // List structure corruption check
			pr_warn("%s: Attempting to double-free node %p (key %llu) in main list. Skipping.\n", __func__, node, node->key);
		} else {
			if (node->value) {
				kmem_cache_free(lsbdd_value_cache, node->value);
				node->value = NULL;
			}
			pr_debug("%s: Freeing node %p (key %llu) from main list\n", __func__, node, node->key);
			kmem_cache_free(lsbdd_node_cache, node);
			last_freed_node_addr = node;
		}
		node = next;
	}
	pr_debug("%s: Finished main list traversal.\n", __func__);

	removed_node_head = (struct lf_list_node *)ATOMIC_LSWAP(&list->removed_stack_head, 0); // null to list->removed_node_head
	node = removed_node_head;
	last_freed_node_addr = NULL;
	pr_debug("%s: Starting removed_stack traversal from node %p\n", __func__, node);
	while (node) {
		next = node->removed_link;

		if (node == last_freed_node_addr) {
			pr_warn("%s: Attempting to double-free node %p (key %llu) in removed_stack. Skipping.\n", __func__, node,
				node->key);
		} else {
			if (node->value) {
				kmem_cache_free(lsbdd_value_cache, node->value);
				node->value = NULL;
				pr_debug("Freed value\n");
			}
			pr_debug("%s: Freeing node %p (key %llu) from removed_stack\n", __func__, node, node->key);
			kmem_cache_free(lsbdd_node_cache, node);
			last_freed_node_addr = node;
		}
		node = next;
	}
	pr_debug("%s: Finished freeing nodes from removed stack.\n", __func__);

	if (list->head) {
		pr_debug("%s: Freeing head node %p\n", __func__, list->head);
		if (list->head != last_freed_node_addr) { // Check before freeing head
			kmem_cache_free(lsbdd_node_cache, list->head);
			last_freed_node_addr = list->head;
		} else {
			pr_warn("%s: Head node %p already freed. Skipping.\n", __func__, list->head);
		}
		list->head = NULL;
	}
	if (list->tail) {
		pr_debug("%s: Freeing tail node %p\n", __func__, list->tail);
		if (list->tail != last_freed_node_addr) { // Check before freeing tail
			kmem_cache_free(lsbdd_node_cache, list->tail);
			// last_freed_node_addr = list->tail; // Not strictly needed after this
		} else {
			pr_warn("%s: Tail node %p already freed. Skipping.\n", __func__, list->tail);
		}
		list->tail = NULL;
	}

	pr_debug("%s: Freeing linked list structure %p\n", __func__, list);
	if (list) { // Check if list itself is not NULL
		kfree(list);
	}
	pr_info("Linked list cleanup finished.\n");
}

struct lf_list_node *lf_list_lookup(struct lf_list *list, sector_t key, struct lf_list_node **left_node_out)
{
	struct lf_list_node *left_node_next_snap = NULL; // Used for detecting the concurrent modifications of the "window"
	struct lf_list_node *right_node = NULL;
	struct lf_list_node *t = NULL, *t_next = NULL;
	u32 retry_count = 0;

	pr_debug("%s: Searching for key %llu in list %p\n", __func__, key, list);
	if (!key) {
		pr_debug("%s: Search for key 0 occured\n", __func__);
		return NULL;
	}

retry_search_outer:
	retry_count++;
	if (retry_count > MAX_LOOKUP_RETRIES) {
		pr_warn("%s: MAX_RETRIES (outer) for key %llu! list %p\n", __func__, key, list);
		return NULL;
	}

	(*left_node_out) = list->head;
	left_node_next_snap = list->head->next;
	t = list->head; // Current node being examined (predecessor candidate)
	t_next = t->next;

	pr_debug("%s: Outer retry %d: t=%p (key %llu), t_next=%p\n", __func__, retry_count, t, t->key, t_next);

	// Find the window (left_node, right_node) where left_node->key < key <= right_node->key
	while (HAS_MARK(t_next) || (t != list->tail && t->key < key)) {
		if (t == STRIP_MARK(t_next)) { // Cause of infinite loops (linked list structure corruption)
			pr_err("%s: Inner loop stalled! t=%p points to itself? t_next=%p. Aborting.\n", __func__, t, t_next);
			return NULL;
		}

		// If t_next is not marked, it's a potential candidate for left_node_next_snap
		if (!HAS_MARK(t_next)) {
			(*left_node_out) = t;
			left_node_next_snap = t_next;
			pr_debug("%s: Inner loop: Updated left_node=%p (key %llu), left_next_snap=%p\n", __func__, *left_node_out,
				 (*left_node_out)->key, left_node_next_snap);
		} else {
			pr_debug("%s: Inner loop: Skipping marked t_next %p (from t=%p)\n", __func__, t_next, t);
		}

		t = STRIP_MARK(t_next); // Move t further

		if (t == list->tail) {
			pr_debug("%s: Inner loop: Reached tail (t == list->tail).\n", __func__);
			break;
		}

		t_next = t->next;
		pr_debug("%s: Inner loop: Advanced t=%p (key %llu), t_next=%p\n", __func__, t, t->key, t_next);
	}
	// After inner loop, 't' is the first node with t->key >= key (or list->tail)
	right_node = t;

	pr_debug("%s: Inner loop finished. right_node=%p (key %llu). Final left_node=%p, left_next_snap=%p\n", __func__, right_node,
		 right_node != list->tail ? right_node->key : (sector_t)-1, *left_node_out, left_node_next_snap);

	// Corruption checks

	if (left_node_next_snap == right_node) {
		pr_debug("%s: Window appears clean (left_next_snap == right_node).\n", __func__);
		// Check if the right_node itself is marked for deletion
		if (right_node != list->tail && HAS_MARK(right_node->next)) {
			pr_debug("%s: right_node %p (key %llu) is marked for deletion. Retrying outer search.\n", __func__, right_node,
				 right_node->key);
			// According to reference logic, we should retry the search if the target is marked.
			goto retry_search_outer;
		}
		pr_debug("%s: Success. Returning right_node %p (key %llu).\n", __func__, right_node,
			 right_node != list->tail ? right_node->key : (sector_t)-1);
		return right_node;
	}
	/** Window is dirty.
	 * This means nodes between (*left_node_out) and right_node were marked/removed, or (*left_node_out)->next was changed concurrently.
     * Attempt to swing (*left_node_out)->next from the old snapshot value (left_node_next_snap) to the currently found successor (right_node).
	 */
	else {
		if (unlikely((*left_node_out == list->head) && (right_node == list->head))) {
			pr_err("%s[Ref]: !!! SAFETY ABORT: Attempted CAS would set head->next = head! ...\n", __func__);
			cpu_relax();
		}
		if (unlikely(*left_node_out == right_node && *left_node_out != list->head)) { // Check for node->next = node
			pr_err("%s[Ref]: !!! SAFETY ABORT: Attempted CAS would set node->next = node! ...\n", __func__);
			cpu_relax();
		}
		pr_debug("%s: Window dirty (left_next_snap %p != right_node %p). Attempting cleanup CAS.\n",
			__func__, left_node_next_snap, right_node);

		// CAS to physically remove intermediate marked nodes.
		if (SYNC_LCAS(&((*left_node_out)->next), left_node_next_snap, right_node) == left_node_next_snap) {
			pr_debug("%s: Cleanup CAS success! List modified. Retrying outer search.\n", __func__);
			goto retry_search_outer;
		} else {
			// CAS failed. Means (*left_node_out)->next was changed by another thread between our read
			// (when we set left_node_next_snap) and the CAS attempt.
			pr_debug("%s: Cleanup CAS failed (concurrent modification). Retrying outer search.\n", __func__);
			goto retry_search_outer;
		}
	}

	pr_err("%s: Reached end of function unexpectedly for key %llu.\n", __func__, key);
	return NULL;
}

struct lf_list_node *lf_list_add(struct lf_list *list, sector_t key, void *val, struct kmem_cache *list_node_cache)
{
	struct lf_list_node *left = NULL, *right = NULL;
	struct lf_list_node *new_node = NULL;

	new_node = node_alloc(key, val, NULL, list_node_cache);

	while (1) {
		right = lf_list_lookup(list, key, &left);
		if (right == NULL) {
			pr_warn("lf_list_add: lf_list_lookup returned NULL for key %llu. Aborting add.\n", key);
			// Free the pre-allocated new_node as it won't be inserted
			kmem_cache_free(list_node_cache, new_node);
			return NULL; // Indicate failure
		}
		if (right != list->tail && right->key == key) {
			pr_debug("lf_list_add: Duplicate key %llu found. Freeing new_node %p.\n", key, new_node);
			kmem_cache_free(list_node_cache, new_node); // Free the unused node
			return NULL;
		}
		new_node->next = right;
		if (SYNC_LCAS(&(left->next), right, new_node) == right) {
			ATOMIC_FAI(&list->size);
			return new_node;
		}
	}

	return new_node;
}

bool lf_list_remove(struct lf_list *list, sector_t key)
{
	struct lf_list_node *left = NULL;
	while (1) {
		struct lf_list_node *right = lf_list_lookup(list, key, &left);
		if (!right) {
			pr_warn("lf_list_remove: lookup failed for key %llu (returned NULL). Cannot remove.\n", key);
			return false;
		}

		if ((right == list->tail) || (right->key != key)) {
			pr_debug("Right %p key %llu \n", right, right->key);
			pr_debug("Left %p key %llu \n", left, left->key);
			return false;
		}

		struct lf_list_node *right_succ = right->next;

		if (HAS_MARK(right_succ)) {
			// Node 'right' is already logically deleted or deletion is in progress.
			pr_debug("lf_list_remove: Node %p (key %llu) already marked. Consider removed.\n", right, right->key);
			return true;
		}

		// Try to mark 'right->next' to logically delete 'right'
		if (SYNC_LCAS(&(right->next), right_succ, MARK_NODE(right_succ)) == right_succ) {
			atomic64_dec(&list->size);
			add_to_removed_stack(list, right); // Add to stack *only on successful first marking*
			return true;
		}
		// CAS failed: right->next changed. Loop and retry.
	}
}
