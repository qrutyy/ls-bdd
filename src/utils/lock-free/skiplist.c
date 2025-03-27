// SPDX-License-Identifier: GPL-2.0-only

/*
 * Originail author: Daniel Vlasenco @spisladqo
 *
 * Last modified by Mikhail Gavrilenko on 11.03.25
 * Changes:
 * Add remove, get_last, get_prev methods
 * Fixed some issues with remove. Modified the TAIL_VALUE and data types that
 * appear in structur. Implement lock-free concurrency. (@chen--oRanGe)
 */

#include "skiplist.h"
#include <linux/random.h>

/**
 * Generates random level for inserting the node.
 * Generator is based on rand + ammount of trailing zero's. It can have a pretty
 * bad behaviour in terms of speed. Can be changed to some easier-to-compute
 * pseudo gen.
 *
 * Uses atomic increment for setting up the max_lvl variable.
 */
static s32 random_levels(struct skiplist *sl)
{
	u32 r = get_random_u32();
	s32 trail_zeros = __builtin_ctz(r);
	s32 levels = (trail_zeros / 2);

	if (levels == 0)
		return 1;
	if (levels > MAX_LVL)
		levels = MAX_LVL;
	if (levels > atomic_read(&sl->max_lvl)) {
		SYNC_INC(&sl->max_lvl);
		levels = atomic_read(&sl->max_lvl);
		pr_debug("Skiplist(random_levels): increased high water mark to %d\n", levels);
	}
	return levels;
}

/**
 * Allocates memory per node.
 * In our case - node is a tower. Node's next - is an array of next nodes.
 * - !size_t array for safe pointer storage.
 */
static struct skiplist_node *node_alloc(sector_t key, void *value, s32 height, struct kmem_cache *sl_cache)
{
	BUG_ON(height <= 0 || height > MAX_LVL);
	struct skiplist_node *node = NULL;
	// decrease of the memory consumption is real (f.e. cache only the skiplist_node size, or create caches for every size)
	node = kmem_cache_zalloc(sl_cache, GFP_KERNEL);
	if (!node)
		goto alloc_fail;

	node->key = key;
	node->value = value;
	node->height = height;
	return node;

alloc_fail:
	kfree(node);
	return NULL;
}

struct skiplist *skiplist_init(struct kmem_cache *sl_cache)
{
	struct skiplist *sl = NULL;

	sl = kzalloc(sizeof(struct skiplist), GFP_KERNEL);
	if (!sl)
		goto alloc_fail;

	atomic_set(&sl->max_lvl, 1);
	sl->head = node_alloc(HEAD_KEY, HEAD_VALUE, MAX_LVL, sl_cache);
	return sl;

alloc_fail:
	kfree(sl);
	return NULL;
}

void skiplist_free(struct skiplist *sl, struct kmem_cache *sl_cache, struct kmem_cache *lsbdd_value_cache)
{
	struct skiplist_node *node = NULL;
	struct skiplist_node *next = NULL;

	node = GET_NODE(sl->head->next[0]);
	while (node) {
		next = STRIP_MARK(node->next[0]);
		kmem_cache_free(lsbdd_value_cache, node->value);
		kmem_cache_free(sl_cache, node);
		node = next;
	}
	kfree(sl);
}

bool skiplist_is_empty(struct skiplist *sl)
{
	return sl->head->next[0] == 0;
}

/**
 * The `find_preds` function searches for nodes in a skiplist that precede and
 * follow a node with a given key, traversing levels from top to bottom. If a
 * node is logically removed (marked), it is either skipped (with `DONT_UNLINK`)
 * or physically removed (with `unlink`). For each node, its key is checked, and
 * if it matches the target key, the node is returned. If the node is not found,
 * `NULL` is returned, and the place for inserting a new node is stored in the
 * `preds` and `succs` arrays.
 */
static struct skiplist_node *find_preds(struct skiplist_node **preds, struct skiplist_node **succs, s32 n, struct skiplist *sl,
					sector_t key, enum unlink unlink)
{
	struct skiplist_node *pred = NULL;
	struct skiplist_node *node = NULL;
	int d = -1;
	size_t next, other = 0;

	pred = sl->head;
	pr_debug("find_preds: searching for key %lld in skiplist (head: %p, max_lvl: %d)\n", key, pred, atomic_read(&sl->max_lvl));

	// Traverse the levels of <sl> from the top level to the bottom
	for (ssize_t level = atomic_read(&sl->max_lvl) - 1; level >= 0; --level) {
		if (!pred) {
			pr_debug("find_preds: pred is NULL at level %ld\n", level);
			BUG();
		}
		if (level > pred->height)
			continue;

		next = pred->next[level];
		pr_debug("Level %zd: pred = %p, next = %zx\n", level, pred, next);

		if (next == 0 && level >= n)
			continue;

		if (HAS_MARK(next)) {
			pr_debug("Level %zd: encountered marked node %p, retrying\n", level, GET_NODE(next));
			BUG_ON(!(level == pred->height - 1 || HAS_MARK(pred->next[level + 1])));
			// retry, because next is about to be removed (ftm is logically removed)
			return find_preds(preds, succs, n, sl, key, unlink);
		}

		node = GET_NODE(next);
		while (node != NULL && level < node->height && node->height <= MAX_LVL) {
			next = node->next[level];
			// A tag means a node is logically removed but not physically unlinked yet
			while (HAS_MARK(next)) {
				if (unlink == DONT_UNLINK) {
					// Skip over logically removed nodes and remove the marks
					node = STRIP_MARK(next);
					pr_debug("Skipping marked node: now at %p\n", node);
					if (node == NULL)
						break;
					next = node->next[level];
				} else {
					// Unlink logically removed nodes.
					pr_debug("Unlinking node: pred = %p, node = %p, new_next = %p\n", pred, node, STRIP_MARK(next));
					other = SYNC_CAS(&pred->next[level], (size_t)node, STRIP_MARK(next));
					if (other == (size_t)node) {
						node = STRIP_MARK(next);
					} else {
						if (HAS_MARK(other)) {
							pr_debug("CAS failed due to marked node, retrying");
							return find_preds(preds, succs, n, sl, key, unlink);
						}
						node = GET_NODE(other);
						pr_debug("Level %zd(mark): GET_NODE(%zx) returned node = %p\n", level, next, node);
					}
					next = (node != NULL) ? node->next[level] : 0;
				}
			}
			if (node == NULL) {
				pr_debug("cringe\n");
				break;
			}

			d = node->key - key;
			if (d > 0) {
				pr_debug("Breaking loop: found node with key %lld > %lld\n", node->key, key);
				break;
			}

			if (d == 0) {
				pr_debug("Key match found at node %p\n", node);
				break;
			}

			pred = node;
			node = GET_NODE(next);
		}

		if (level < n) {
			if (preds != NULL) {
				pr_debug("ADDED PRED = %p\n", pred);
				preds[level] = pred;
			}
			if (succs != NULL)
				succs[level] = node;
		}
	}

	if (d == 0) {
		pr_debug("find_preds: found matching node %p in skiplist, pred is %p\n", node, pred);
		return node;
	}

	pr_debug("find_preds: key %lld NOT FOUND (pred = %p, pred_key = %lld), returning NULL\n", key, pred, pred ? pred->key : -1);
	return NULL;
}

struct skiplist_node *skiplist_find_node(struct skiplist *sl, sector_t key)
{
	struct skiplist_node *node = NULL;

	node = find_preds(NULL, NULL, 0, sl, key, DONT_UNLINK);
	if (!node)
		pr_debug("Skiplist(sl_lookup): no node in the skiplist matched the key");

	return node;
}

/**
 * Gets last node just by iterating at the level 0.
 * If some node has mark -> remove the mark and skip it.
 */

sector_t skiplist_last(struct skiplist *sl)
{
	return sl->last_key;
}

static void *update_node(struct skiplist_node *node, void *new_val)
{
	void *old_val = NULL;

	old_val = node->value;

	// If the node's value is 0 it means another thread removed the node out from under us.
	if (!old_val) {
		pr_debug("Skiplist(update_node): lost a race to another thread removing the node. retry");
		return NULL;
	}

	/** Use a CAS and not a SWAP. If the CAS fails it means another thread removed the node or updated its value.
	  * If another thread removed the node but it is not unlinked yet and we used a SWAP, we could replace 0 with our value.
	  * Then another thread that is updating the value could think it succeeded and return our value even though it should return 0.
	  */
	if (old_val == SYNC_CAS(&node->value, old_val, new_val)) {
		pr_debug("Skilist(update_node): the CAS succeeded. updated the value of the node\n");
		return old_val;
	}
	pr_debug("Skiplist(update_node): lost a race. the CAS failed. another thread changed the node's value");

	return update_node(node, new_val); // tail call (retry)
}

struct skiplist_node *skiplist_insert(struct skiplist *sl, sector_t key, void *value, struct kmem_cache *sl_cache)
{
	pr_debug("Skiplist(insert): key %lld skiplist %p\n", key, sl);
	pr_debug("Skiplist(insert): new value %p\n", value);
	BUG_ON(!value);

	struct skiplist_node *preds[MAX_LVL];
	struct skiplist_node *nexts[MAX_LVL];
	struct skiplist_node *new_node = NULL;
	struct skiplist_node *old_node = NULL;
	struct skiplist_node *pred = NULL;
	void *ret_val = NULL;
	size_t other, old_next, next = 0;
	s32 n;

	if (key > sl->last_key)
		sl->last_key = key;
	pr_debug("last: %lld\n", sl->last_key);

	n = random_levels(sl);
	old_node = find_preds(preds, nexts, n, sl, key, ASSIST_UNLINK);

	// If there is already an node in the skiplist that matches the key just update its value.
	if (old_node != NULL) {
		ret_val = update_node(old_node, value);
		if (ret_val != 0)
			return ret_val;

		// If we lose a race with a thread removing the node we tried to update then we have to retry.
		return skiplist_insert(sl, key, value, sl_cache); // tail call
	}

	pr_debug("Skiplist(insert): attempting to insert a new node between %p and %p, height %d\n", preds[0], nexts[0], n);

	new_node = node_alloc(key, value, n, sl_cache);
	if (!(new_node && new_node->value))
		goto mem_err;

	// Set <new_node>'s next pointers to their proper values
	//	next = new_node->next[0] = (size_t)nexts[0];
	for (size_t level = 0; level < new_node->height; ++level) {
		new_node->next[level] = (size_t)nexts[level];
		pr_debug("Skiplist(insert):assigning new %p at level %zd\n", nexts[level], level);
	}

	/** Link <new_node> into <sl> from the bottom level up.
	  * After <new_node> is inserted into the bottom level it is officially part of the skiplist.
	  * Can lead to bugs i guess...
	  */
	pred = (struct skiplist_node *)preds[0];
	next = (size_t)nexts[0];
	new_node->next[0] = (size_t)next;

	pr_debug("Before cas: next = %zx, new_node = %p\n", next, new_node);
	other = SYNC_CAS(&pred->next[0], next,
			 new_node); // does it change only the lower one?
	if (other != next) {
		pr_debug("Skiplist(insert): failed to change pred's link: expected %zx found %zx\n", next, other);
		kfree(new_node);
		return skiplist_insert(sl, key, value, sl_cache); // retry
	}
	pr_debug("Skiplist(insert): other = %zx new_node = %p next = %zx, pred = %p\n", other, new_node, next, pred);
	pr_debug("Skiplist(insert): successfully inserted a new node %p at the bottom level\n", new_node);
	pr_debug("Skiplist(insert): pred[0] = %p, pred[1] = %p\n", preds[0], preds[1]);
	pr_debug("Skiplist(insert): pred->next[0] = %zx, pred->next[1] = %zx\n", (&pred[1])->next[0], (&pred[1])->next[1]);

	// Basically - link the prev nodes, that were found in find_preds with
	// the new "nexts".
	BUG_ON(new_node->height > MAX_LVL);
	for (size_t level = 1; level < new_node->height; ++level) {
		pr_debug("Skiplist(insert): inserting the new node %p at level %ld\n", new_node, level);
		do {
			pred = preds[level];
			next = (size_t)nexts[level];
			BUG_ON(!(new_node->next[level] == (size_t)nexts[level] || new_node->next[level] == MARK_NODE(nexts[level])));
			pr_debug("Skiplist(insert): attempting to insert the new node between %p and %p\n", pred, nexts[level]);

			other = SYNC_CAS(&pred->next[level], next, new_node);
			/** despite other info, ibm sets the return as "initial value of the
			  * variable that __p points to successfully linked <new_node> with
			  * prev at the current <level> */
			if (other == next)
				break;

			pr_debug("Skiplist(insert): lost a race. failed to "
				 "change pred's link. expected %p found %ld\n",
				 nexts[level], other);

			// Find <new_node>'s new preds and nexts. (retry the insertion)
			find_preds(preds, nexts, new_node->height, sl, key, ASSIST_UNLINK);

			for (size_t i = level; i < new_node->height; ++i) {
				old_next = new_node->next[i];
				if ((size_t)nexts[i] == old_next)
					continue;

				/** Update <new_node>'s inconsistent next pointer before trying again. Use a CAS so if another
				  * thread is trying to remove the new node concurrently we do not stomp on the mark it
				  * places on the node. */
				pr_debug("Skiplist(insert): attempting to update "
					 "the new node's link from %ld to %p\n",
					 old_next, nexts[i]);
				other = SYNC_CAS(&new_node->next[i], old_next, nexts[i]);
				BUG_ON(!(other == old_next || other == MARK_NODE(old_next)));

				// If another thread is removing this node we
				// can stop linking it into to skiplist
				if (HAS_MARK(other)) {
					find_preds(NULL, NULL, 0, sl, key,
						   FORCE_UNLINK); // see comment below
					return 0;
				}
			}
		} while (1);
	}

	/** In case another thread was in the process of removing the <new_node> while we were added it,
	 * we have to make sure it is completely unlinked before we return.
	 * We might have lost a race and inserted the new node at some level after the other thread
	 * thought it was fully removed.
	 * That is a problem because once a thread thinks it completely unlinks a node it queues it to be freed */
	if (HAS_MARK(new_node->next[new_node->height - 1])) {
		find_preds(NULL, NULL, 0, sl, key, FORCE_UNLINK);
	}

	return 0;

mem_err:
	pr_warn("Failed to allocate node, retrying\n");
	return skiplist_insert(sl, key, value, sl_cache);
}

void skiplist_remove(struct skiplist *sl, sector_t key, struct kmem_cache *lsbdd_value_cache)
{
	struct skiplist_node *preds[MAX_LVL];
	struct skiplist_node *node = NULL;
	size_t old_next = 0;
	void *val = 0;
	ssize_t level = 0;
	pr_debug("Skiplist(remove): removing node with key %lld from skiplist %p\n", key, sl);

	node = find_preds(preds, NULL, atomic_read(&sl->max_lvl), sl, key, ASSIST_UNLINK);
	if (node == NULL) {
		pr_debug("Skiplist(remove: remove failed, an node with a matching key does not exist in the skiplist");
		return;
	}

	/** Mark <node> at each level of <sl> from the top down.
	  * If multiple threads try to concurrently remove the same node only one of them should succeed.
	  * Marking the bottom level establishes which of them succeeds.
	  */
	old_next = 0;
	for (level = node->height - 1; level >= 0; --level) {
		ssize_t next;
		old_next = node->next[level];
		do {
			pr_debug("Skiplist(remove): marking node at level %ld (next %ld)\n", level, old_next);
			next = old_next;
			old_next = SYNC_CAS(&node->next[level], next, MARK_NODE((struct skiplist_node *)next));

			if (HAS_MARK(old_next)) {
				pr_debug("Skiplist(remove): %p is already marked for removal by another thread (next %ld)\n", node,
					 old_next);
				if (level == 0)
					return;
				break;
			}
		} while (next != old_next); // loop is necessary, bc CAS can fail
	}

	/* Atomically swap out the node's value in case another thread is updating the node while we are removing it.
	  * This establishes which operation occurs first logically, the update or the remove.
	  */
	val = SYNC_SWAP(&node->value, 0);
	pr_debug("Skiplist(remove): replaced node %p's value with 0\n", node);

	// unlink the node
	find_preds(NULL, NULL, 0, sl, key, FORCE_UNLINK);
	kmem_cache_free(lsbdd_value_cache, val);
	kfree(node); // was a rcu_defer_free;

	return;
}
struct skiplist_node *skiplist_prev(struct skiplist *sl, sector_t key, sector_t *prev_key)
{
	struct skiplist_node *pred = sl->head;
	struct skiplist_node *node = NULL;
	size_t next;

	for (ssize_t level = atomic_read(&sl->max_lvl) - 1; level >= 0; --level) {
		while (1) {
			next = pred->next[level];
			node = GET_NODE(next);

			if (!node || node->key >= key)
				break;

			pred = node;
		}
	}

	if (pred == sl->head)
		return NULL;

	if (prev_key)
		*prev_key = pred->key;
	return pred;
}
