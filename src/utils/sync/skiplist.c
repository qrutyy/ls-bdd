// SPDX-License-Identifier: GPL-2.0-only

/*
 * Lazy skip list, see skiplist.h.
 *
 * Locking rules:
 * - a node's lock protects its |marked| flag, its |value| and its |next| links;
 * - the locks of one operation are taken from level 0 upwards. The predecessors
 *   of the levels go down the keys, so every operation takes its locks in
 *   non-increasing key order and there is no deadlock (a removal locks the
 *   victim first, its key is greater than that of its predecessors);
 * - nothing is allocated under a lock.
 */

#include "skiplist.h"
#include <linux/random.h>
#include <linux/slab.h>
#include <linux/lockdep.h>

/* The nodes of one operation are locked together, lockdep cannot tell them apart. */
static void node_lock(struct skiplist_node *node)
{
	spin_lock(&node->lock);
}

static void node_unlock(struct skiplist_node *node)
{
	spin_unlock(&node->lock);
}

static struct skiplist_node *node_alloc(sector_t key, void *value, s32 height, struct kmem_cache *node_cache)
{
	struct skiplist_node *node = NULL;

	BUG_ON(height <= 0 || height > MAX_LVL || !node_cache);

	node = kmem_cache_zalloc(node_cache, GFP_KERNEL);
	if (!node)
		return NULL;

	node->key = key;
	node->value = value;
	node->height = height;
	spin_lock_init(&node->lock);
	lockdep_set_novalidate_class(&node->lock);

	return node;
}

/*
 * The height of a new node: the same distribution as in the lock-free skip
 * list (a quarter of the nodes go one level up). The number of levels in use
 * grows by one level at a time, and is not lower than the returned height once
 * this returns.
 */
static s32 random_height(struct skiplist *sl)
{
	u32 r = get_random_u32();
	s32 height = (r ? __builtin_ctz(r) : 32) / 2;
	s32 cur;

	if (height == 0)
		return 1;
	if (height > MAX_LVL)
		height = MAX_LVL;

	cur = atomic_read(&sl->max_lvl);
	if (height > cur + 1)
		height = cur + 1;
	while (height > cur) {
		if (atomic_try_cmpxchg(&sl->max_lvl, &cur, height))
			break;
	}

	return height;
}

static void update_last_key(struct skiplist *sl, sector_t key)
{
	s64 cur = atomic64_read(&sl->last_key);

	while ((u64)cur < key && !atomic64_try_cmpxchg(&sl->last_key, &cur, key))
		;
}

static void push_removed(struct skiplist *sl, struct skiplist_node *node)
{
	struct skiplist_node *old = READ_ONCE(sl->removed_head);

	do {
		WRITE_ONCE(node->removed_link, old);
	} while (!try_cmpxchg(&sl->removed_head, &old, node));
}

static void push_old_value(struct skiplist *sl, struct skiplist_old_value *holder)
{
	struct skiplist_old_value *old = READ_ONCE(sl->old_values);

	do {
		WRITE_ONCE(holder->next, old);
	} while (!try_cmpxchg(&sl->old_values, &old, holder));
}

/*
 * Walks down from the top level, remembering the last node smaller than |key|
 * and the first one not smaller on every level. Takes no locks.
 *
 * @return the highest level the key was found on, -1 if it was not found
 */
static s32 find(struct skiplist *sl, sector_t key, struct skiplist_node **preds, struct skiplist_node **succs)
{
	struct skiplist_node *pred = sl->head;
	struct skiplist_node *curr = NULL;
	s32 found = -1;
	s32 lvl;

	for (lvl = atomic_read(&sl->max_lvl) - 1; lvl >= 0; --lvl) {
		curr = READ_ONCE(pred->next[lvl]);
		while (curr != sl->tail && curr->key < key) {
			pred = curr;
			curr = READ_ONCE(pred->next[lvl]);
		}
		if (found < 0 && curr != sl->tail && curr->key == key)
			found = lvl;
		preds[lvl] = pred;
		succs[lvl] = curr;
	}

	return found;
}

/* The node with the greatest key smaller than |key|, the head if there is none. */
static struct skiplist_node *find_smaller(struct skiplist *sl, sector_t key)
{
	struct skiplist_node *pred = sl->head;
	struct skiplist_node *curr = NULL;
	s32 lvl;

	for (lvl = atomic_read(&sl->max_lvl) - 1; lvl >= 0; --lvl) {
		curr = READ_ONCE(pred->next[lvl]);
		while (curr != sl->tail && curr->key < key) {
			pred = curr;
			curr = READ_ONCE(pred->next[lvl]);
		}
	}

	return pred;
}

/* Unlocks the distinct predecessors of levels 0..|highest|. */
static void unlock_preds(struct skiplist_node **preds, s32 highest)
{
	struct skiplist_node *last = NULL;
	s32 lvl;

	for (lvl = 0; lvl <= highest; ++lvl) {
		if (preds[lvl] != last) {
			node_unlock(preds[lvl]);
			last = preds[lvl];
		}
	}
}

/*
 * Locks the predecessors of levels 0..|height|-1 and checks that nothing moved
 * since find(): none is removed and each still points at |expect[lvl]|.
 *
 * @param expect - the node every predecessor must still link to on its level
 *                 (an insertion passes the successors, a removal the victim)
 * @param highest - the level up to which the locks are held
 */
static bool lock_and_validate(struct skiplist_node **preds, struct skiplist_node **expect, struct skiplist_node *victim,
			      s32 height, s32 *highest)
{
	struct skiplist_node *last = NULL;
	struct skiplist_node *succ = NULL;
	bool valid = true;
	s32 lvl;

	*highest = -1;
	for (lvl = 0; valid && lvl < height; ++lvl) {
		if (preds[lvl] != last) {
			node_lock(preds[lvl]);
			last = preds[lvl];
			*highest = lvl;
		}
		if (victim) {
			/* the victim is marked by its remover, only the links matter */
			valid = !READ_ONCE(preds[lvl]->marked) && READ_ONCE(preds[lvl]->next[lvl]) == victim;
		} else {
			succ = expect[lvl];
			valid = !READ_ONCE(preds[lvl]->marked) && !READ_ONCE(succ->marked) &&
				READ_ONCE(preds[lvl]->next[lvl]) == succ;
		}
	}

	return valid;
}

struct skiplist *skiplist_init(struct kmem_cache *lsbdd_node_cache)
{
	struct skiplist *sl = NULL;
	s32 lvl;

	BUG_ON(!lsbdd_node_cache);

	sl = kzalloc(sizeof(struct skiplist), GFP_KERNEL);
	if (!sl)
		return NULL;

	sl->head = node_alloc(0, NULL, MAX_LVL, lsbdd_node_cache);
	sl->tail = node_alloc(U64_MAX, NULL, MAX_LVL, lsbdd_node_cache);
	if (!sl->head || !sl->tail)
		goto mem_err;

	for (lvl = 0; lvl < MAX_LVL; ++lvl)
		sl->head->next[lvl] = sl->tail;
	sl->head->fully_linked = true;
	sl->tail->fully_linked = true;

	atomic_set(&sl->max_lvl, 1);
	atomic64_set(&sl->last_key, 0);

	return sl;

mem_err:
	if (sl->head)
		kmem_cache_free(lsbdd_node_cache, sl->head);
	if (sl->tail)
		kmem_cache_free(lsbdd_node_cache, sl->tail);
	kfree(sl);
	return NULL;
}

struct skiplist_node *skiplist_find_node(struct skiplist *sl, sector_t key)
{
	struct skiplist_node *pred = NULL;
	struct skiplist_node *curr = NULL;
	s32 lvl;

	BUG_ON(!sl);

	pred = sl->head;
	for (lvl = atomic_read(&sl->max_lvl) - 1; lvl >= 0; --lvl) {
		curr = READ_ONCE(pred->next[lvl]);
		while (curr != sl->tail && curr->key < key) {
			pred = curr;
			curr = READ_ONCE(pred->next[lvl]);
		}
		if (curr != sl->tail && curr->key == key) {
			if (smp_load_acquire(&curr->fully_linked) && !READ_ONCE(curr->marked))
				return curr;
			return NULL;
		}
	}

	return NULL;
}

struct skiplist_node *skiplist_insert(struct skiplist *sl, sector_t key, void *value, struct kmem_cache *lsbdd_node_cache,
				      struct kmem_cache *lsbdd_value_cache)
{
	struct skiplist_node *preds[MAX_LVL];
	struct skiplist_node *succs[MAX_LVL];
	struct skiplist_node *new_node = NULL;
	struct skiplist_node *found = NULL;
	struct skiplist_old_value *holder = NULL;
	s32 height = 0;
	s32 highest = -1;
	s32 lfound = 0;
	s32 lvl = 0;

	BUG_ON(!sl || !lsbdd_node_cache);
	if (WARN_ON(!value))
		return NULL;

	height = random_height(sl);

	for (;;) {
		lfound = find(sl, key, preds, succs);

		if (lfound >= 0) {
			/* The key is there: replace its value. */
			found = succs[lfound];
			if (READ_ONCE(found->marked)) {
				cpu_relax(); /* being removed, wait for it to go */
				continue;
			}
			while (!smp_load_acquire(&found->fully_linked))
				cpu_relax();

			if (!holder) {
				holder = kmalloc(sizeof(*holder), GFP_KERNEL);
				if (!holder)
					goto mem_err;
			}

			node_lock(found);
			if (READ_ONCE(found->marked)) {
				node_unlock(found);
				continue;
			}
			holder->value = found->value;
			/* lookups read the value without the lock: publish its contents first */
			smp_store_release(&found->value, value);
			node_unlock(found);

			push_old_value(sl, holder);
			if (new_node)
				kmem_cache_free(lsbdd_node_cache, new_node);
			return found;
		}

		if (!new_node) {
			new_node = node_alloc(key, value, height, lsbdd_node_cache);
			if (!new_node)
				goto mem_err;
		}

		if (!lock_and_validate(preds, succs, NULL, height, &highest)) {
			unlock_preds(preds, highest);
			continue;
		}

		for (lvl = 0; lvl < height; ++lvl)
			WRITE_ONCE(new_node->next[lvl], succs[lvl]);
		/* The node becomes reachable bottom-up, and visible when the last level is. */
		for (lvl = 0; lvl < height; ++lvl)
			smp_store_release(&preds[lvl]->next[lvl], new_node);
		smp_store_release(&new_node->fully_linked, true);

		unlock_preds(preds, highest);
		update_last_key(sl, key);
		kfree(holder);
		return new_node;
	}

mem_err:
	if (new_node)
		kmem_cache_free(lsbdd_node_cache, new_node);
	kfree(holder);
	return NULL;
}

void skiplist_remove(struct skiplist *sl, sector_t key, struct kmem_cache *lsbdd_value_cache)
{
	struct skiplist_node *preds[MAX_LVL];
	struct skiplist_node *succs[MAX_LVL];
	struct skiplist_node *victim = NULL;
	bool is_marked = false;
	s32 height = 0;
	s32 highest = -1;
	s32 lfound = 0;
	s32 lvl = 0;

	BUG_ON(!sl);

	for (;;) {
		lfound = find(sl, key, preds, succs);

		if (!is_marked) {
			if (lfound < 0)
				return;

			victim = succs[lfound];
			/* Only a fully linked node, found on its top level, is in the set. */
			if (!smp_load_acquire(&victim->fully_linked) || victim->height != lfound + 1 || READ_ONCE(victim->marked))
				return;

			node_lock(victim);
			if (READ_ONCE(victim->marked)) {
				node_unlock(victim);
				return;
			}
			WRITE_ONCE(victim->marked, true); /* the logical removal */
			is_marked = true;
			height = victim->height;
		}

		if (!lock_and_validate(preds, succs, victim, height, &highest)) {
			unlock_preds(preds, highest);
			continue;
		}

		for (lvl = height - 1; lvl >= 0; --lvl)
			smp_store_release(&preds[lvl]->next[lvl], READ_ONCE(victim->next[lvl]));

		node_unlock(victim);
		unlock_preds(preds, highest);

		push_removed(sl, victim);
		return;
	}
}

struct skiplist_node *skiplist_prev(struct skiplist *sl, sector_t key, sector_t *prev_key)
{
	struct skiplist_node *pred = NULL;

	BUG_ON(!sl);

	for (;;) {
		pred = find_smaller(sl, key);
		if (pred == sl->head)
			return NULL;

		/* A node being inserted or removed is not in the set, go below it. */
		if (smp_load_acquire(&pred->fully_linked) && !READ_ONCE(pred->marked)) {
			if (prev_key)
				*prev_key = pred->key;
			return pred;
		}
		key = pred->key;
	}
}

sector_t skiplist_last(struct skiplist *sl)
{
	BUG_ON(!sl);
	return atomic64_read(&sl->last_key);
}

bool skiplist_is_empty(struct skiplist *sl)
{
	BUG_ON(!sl);
	return READ_ONCE(sl->head->next[0]) == sl->tail;
}

void skiplist_print(struct skiplist *sl)
{
	struct skiplist_node *curr = NULL;

	BUG_ON(!sl);

	pr_cont("head->");
	for (curr = READ_ONCE(sl->head->next[0]); curr != sl->tail; curr = READ_ONCE(curr->next[0]))
		pr_cont("(%llu-%p)->", curr->key, curr->value);
	pr_cont("tail\n");
}

void skiplist_free(struct skiplist *sl, struct kmem_cache *lsbdd_node_cache, struct kmem_cache *lsbdd_value_cache)
{
	struct skiplist_node *node = NULL;
	struct skiplist_node *next = NULL;
	struct skiplist_old_value *old = NULL;
	struct skiplist_old_value *old_next = NULL;

	BUG_ON(!sl || !lsbdd_node_cache || !lsbdd_value_cache);

	for (node = sl->head->next[0]; node != sl->tail; node = next) {
		next = node->next[0];
		if (node->value)
			kmem_cache_free(lsbdd_value_cache, node->value);
		kmem_cache_free(lsbdd_node_cache, node);
	}

	for (node = sl->removed_head; node; node = next) {
		next = node->removed_link;
		if (node->value)
			kmem_cache_free(lsbdd_value_cache, node->value);
		kmem_cache_free(lsbdd_node_cache, node);
	}

	for (old = sl->old_values; old; old = old_next) {
		old_next = old->next;
		if (old->value)
			kmem_cache_free(lsbdd_value_cache, old->value);
		kfree(old);
	}

	kmem_cache_free(lsbdd_node_cache, sl->head);
	kmem_cache_free(lsbdd_node_cache, sl->tail);
	kfree(sl);
}
