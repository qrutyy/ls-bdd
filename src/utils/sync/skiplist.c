// SPDX-License-Identifier: GPL-2.0-only

/*
 * Originail author: Daniel Vlasenco @spisladqo
 *
 * Modified by Mikhail Gavrilenko on 15.11.24
 * Changes: add remove, get_last, get_prev methods
 * Fixed some issues with remove. Modified the TAIL_VALUE and data types that appear in structur.
 */

#include "skiplist.h"

static void free_node_full(struct skiplist_node *node)
{
	struct skiplist_node *temp;

	while (node) {
		temp = node->lower;
		kfree(node);
		node = temp;
	}
	return;
}

static struct skiplist_node *create_node_tall(sector_t key, void **value,
								s32 h)
{
	struct skiplist_node *last;
	struct skiplist_node *curr;
	s32 curr_h;

	last = NULL;
	for (curr_h = 0; curr_h < h; ++curr_h) {
		curr = kzalloc(sizeof(*curr), GFP_KERNEL);
		if (!curr)
			goto alloc_fail;

		curr->key = key;
		curr->value = value;
		curr->lower = last;
		last = curr;
	}

	return curr;

alloc_fail:
	free_node_full(last);

	return NULL;
}

static struct skiplist_node *create_node(sector_t key, void *value)
{
	return create_node_tall(key, value, 1);
}

struct skiplist *skiplist_init(void)
{
	struct skiplist *sl;
	struct skiplist_node *head;
	struct skiplist_node *tail;

	sl = kzalloc(sizeof(*sl), GFP_KERNEL);
	head = create_node(HEAD_KEY, HEAD_VALUE);
	tail = create_node(TAIL_KEY, TAIL_VALUE);
	if (!sl || !head || !tail)
		goto alloc_fail;

	sl->head = head;
	sl->head_lvl = 0;
	sl->max_lvl = MAX_LVL;
	head->next = tail;

	return sl;

alloc_fail:
	kfree(sl);
	kfree(head);
	kfree(tail);
	return NULL;
}

struct skiplist_node *skiplist_find_node(struct skiplist *sl, sector_t key)
{
	struct skiplist_node *curr = sl->head;

	while (curr) {
		if (curr->next->key == key)
			return curr->next;
		else if (curr->next && curr->next->key && curr->next->key < key)
			curr = curr->next;
		else
			curr = curr->lower;
	}

	return NULL;
}

static s32 move_head_and_tail_up(struct skiplist *sl, int lvls_up)
{
	struct skiplist_node *head_ext;
	struct skiplist_node *tail_ext;
	struct skiplist_node *curr;
	struct skiplist_node *temp;

	head_ext = create_node_tall(HEAD_KEY, HEAD_VALUE, lvls_up);
	tail_ext = create_node_tall(TAIL_KEY, TAIL_VALUE, lvls_up);

	if (!head_ext || !tail_ext)
		goto alloc_fail;

	curr = head_ext;
	temp = tail_ext;
	while (curr && temp) {
		curr->next = temp;
		if (!curr->lower || !temp->lower)
			break;

		curr = curr->lower;
		temp = temp->lower;
	}

	curr->lower = sl->head;
	temp->lower = skiplist_find_node(sl, TAIL_KEY);
	sl->head = head_ext;

	return 0;

alloc_fail:
	free_node_full(head_ext);
	free_node_full(tail_ext);

	return -ENOMEM;
}

static s32 move_up_if_lvl_nex(struct skiplist *sl, int lvl)
{
	u32 diff;
	s32 ret;

	if (lvl <= sl->head_lvl || lvl > sl->max_lvl)
		return 0;

	diff = lvl - sl->head_lvl;
	ret = move_head_and_tail_up(sl, diff);
	if (ret) {
		pr_err("Skiplist: failed to move head and tail up\n");
		return ret;
	}
	sl->head_lvl = lvl;

	return 0;
}

static s32 flip_coin(void)
{
	return get_random_u8() % 2;
}

static s32 get_random_lvl(int max)
{
	s32 lvl = 0;

	while ((lvl < max) && flip_coin())
		lvl++;

	return lvl;
}

static void get_prev_nodes(sector_t key, struct skiplist *sl,
			struct skiplist_node **buf, s32 lvl)
{
	struct skiplist_node *curr;
	s32 curr_lvl;

	curr = sl->head;
	curr_lvl = sl->head_lvl;
	while (curr && curr_lvl >= 0) {
		if (curr->next->key < key) {
			curr = curr->next;
		} else {
			if (curr_lvl <= lvl)
				buf[curr_lvl] = curr;
			--curr_lvl;
			curr = curr->lower;
		}
	}
}

static struct skiplist_node *skiplist_insert_at_lvl(sector_t key,
		void *value, struct skiplist *sl, s32 lvl)
{
	struct skiplist_node *prev[MAX_LVL+1];
	struct skiplist_node *new;
	struct skiplist_node *temp;
	s32 i;

	get_prev_nodes(key, sl, prev, lvl);
	temp = NULL;
	for (i = 0; i <= lvl; ++i) {
		new = create_node(key, value);
		if (!new)
			goto fail;
		new->next = prev[i]->next;
		new->lower = temp;
		prev[i]->next = new;
		temp = new;
	}

	return new;
fail:
	for (i = i - 1; i >= 0; --i) {
		new = prev[i]->next;
		prev[i]->next = new->next;
		kfree(new);
	}

	return ERR_PTR(-ENOMEM);
}

struct skiplist_node *skiplist_add(struct skiplist *sl, sector_t key, void *value)
{
	struct skiplist_node *old;
	struct skiplist_node *new;
	s32 lvl;
	s32 err;

	old = skiplist_find_node(sl, key);
	if (old)
		return old;

	lvl = get_random_lvl(sl->max_lvl);
	err = move_up_if_lvl_nex(sl, lvl);
	if (err)
		goto fail;

	new = skiplist_insert_at_lvl(key, value, sl, lvl);
	if (IS_ERR(new))
		goto fail;

	return new;

fail:
	return ERR_PTR(err);
}

void skiplist_free(struct skiplist *sl)
{
	struct skiplist_node *curr;
	struct skiplist_node *next;
	struct skiplist_node *tofree;
	struct skiplist_node *tofree_stack[MAX_LVL + 1];
	s32 stack_i;

	if (!sl)
		return;

	stack_i = 0;
	tofree_stack[stack_i++] = sl->head;

	while (stack_i > 0 && tofree_stack[stack_i]) {
		tofree = tofree_stack[stack_i];

		curr = tofree;
		while (curr) {
			next = curr->next;
			if (!next)
				break;

			if (next->key < tofree_stack[stack_i]->key)
				tofree_stack[++stack_i] = next;

			curr = curr->lower;
		}

		free_node_full(tofree);
		tofree_stack[stack_i--] = NULL;
	}

	kfree(sl);
}

void skiplist_print(struct skiplist *sl)
{
	struct skiplist_node *curr;
	struct skiplist_node *head;

	head = sl->head;
	while (head) {
		curr = head;
		while (curr) {
			if (curr->key == HEAD_KEY && curr->value == HEAD_VALUE)
				pr_cont("head->");
			else if (curr->key == TAIL_KEY && curr->value == TAIL_VALUE)
				pr_cont("tail->");
			else
				pr_cont("(%llu-%p)->", curr->key, curr->value);

			curr = curr->next;
		}
		pr_cont("\n");
		head = head->lower;
	}
}


void skiplist_remove(struct skiplist *sl, sector_t key)
{
	if (!(sl && sl->head))
		return;

	struct skiplist_node *curr = sl->head;
	struct skiplist_node *prev[MAX_LVL + 1];
	s32 i;

	for (i = sl->head_lvl; i >= 0; --i) {
		while (curr->next && curr->next->key < key)
			curr = curr->next;

		prev[i] = curr;

		if (curr->lower)
			curr = curr->lower;
	}

	curr = prev[0]->next;

	if (curr && curr->key == key) {
		for (i = 0; i <= sl->head_lvl; ++i) {
			if (prev[i]->next == curr)
				prev[i]->next = curr->next;
			curr = prev[i]->next;
		}

		while (sl->head_lvl > 0 && !sl->head->next) {
			struct skiplist_node *old_head = sl->head;

			sl->head = sl->head->lower;
			kfree(old_head);
			--sl->head_lvl;
		}

		return;
	}
}

struct skiplist_node *skiplist_last(struct skiplist *sl)
{
	struct skiplist_node *curr = sl->head;

	while (curr->lower)
		curr = curr->lower;

	while (curr->next && curr->next->key && curr->next->key != TAIL_KEY)
		curr = curr->next;

	return curr;
}


struct skiplist_node *skiplist_prev(struct skiplist *sl, sector_t key, sector_t *prev_key)
{
	struct skiplist_node *curr = sl->head;

	while (curr) {
		while (curr->next && curr->next->key < key)
			curr = curr->next;

		if (!curr->lower) {
			*prev_key = curr->key;
			return curr;
		}

		curr = curr->lower;
	}

	return NULL;
}
