// SPDX-License-Identifier: GPL-2.0-only

#include <linux/err.h>
#include <linux/log2.h>
#include <linux/minmax.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <kunit/static_stub.h>
#include <kunit/visibility.h>

#include "ebr.h"
#include "tests/include_static/__static_kunit_lsv_ht.h"

/* An insertion is about to link its cell; KUnit stalls it here. */
VISIBLE_IF_KUNIT void ht_kunit_cell_linking(struct lsv_lf_list_node *cell)
{
	KUNIT_STATIC_STUB_REDIRECT(ht_kunit_cell_linking, cell);
}

/* one cell for every entry of the table: a byte here is a byte per entry */
static_assert(sizeof(struct lsv_lf_list_node) == 32);
static_assert(sizeof(struct lsv_lf_list) == sizeof(struct lsv_lf_list_node));

/*
 * Cells still linked were never retired, whatever their tags, so every cell
 * reachable from the head is freed here; retired ones are freed by ebr_destroy().
 */
static void lf_list_free(struct lsv_ht *ht, struct lsv_lf_list *list)
{
	struct lsv_lf_list_node *node;
	struct lsv_lf_list_node *next;

	for (node = lsv_ht_strip_mark(list->head.next); node != &ht->tail; node = next) {
		next = lsv_ht_strip_mark(node->next);
		ht->ops->node_dealloc(node, ht->ctx);
	}
}

/*
 * Harris find. Returns the first cell whose key is not below |key| and its
 * live predecessor, after physically unlinking the deleted cells between
 * them. The unlinking participant retires the cells it bypassed. Every cell
 * the walk reaches is protected by the critical region of the operation.
 */
static struct lsv_lf_list_node *lf_list_lookup(struct lsv_ht *ht, struct lsv_lf_list *list,
					       u64 key, struct lsv_lf_list_node **left_out,
					       int tid)
{
	struct lsv_lf_list_node *tail = &ht->tail;
	u32 retry_count = 0;
	struct lsv_lf_list_node *left_next_snap;
	struct lsv_lf_list_node *t_next;
	struct lsv_lf_list_node *right;
	struct lsv_lf_list_node *t;

	if (lsv_ht_key_is_reserved(key))
		return NULL;

retry:
	if (++retry_count > LSV_HT_MAX_LOOKUP_RETRIES)
		return NULL;

	*left_out = &list->head;
	t = &list->head;
	/* one load per cell: every decision below must use the same value */
	t_next = READ_ONCE(t->next);
	left_next_snap = t_next;

	while (lsv_ht_has_mark(t_next) || (t != tail && t->key < key)) {
		if (!lsv_ht_has_mark(t_next)) {
			*left_out = t;
			left_next_snap = t_next;
		}

		t = lsv_ht_strip_mark(t_next);
		if (t == tail)
			break;
		ht_kunit_cell_reached(t);
		t_next = READ_ONCE(t->next);
	}

	right = t;

	if (left_next_snap == right) {
		if (right != tail && lsv_ht_has_mark(READ_ONCE(right->next)))
			goto retry;
		return right;
	}

	if (lsv_ht_lcas(&(*left_out)->next, left_next_snap, right) == left_next_snap)
		ebr_retire_span(ht->ebr, tid, left_next_snap, right);

	goto retry;
}

/*
 * A value's reference count and its presence in the list change separately:
 * the last put drops the count to zero before the key is removed, so a zero
 * count means the value is being torn down and must not be resurrected.
 */
static bool lf_node_ref_get(struct lsv_ht *ht, struct lsv_lf_list_node *node)
{
	if (!ht->ops->value_get)
		return true;

	return ht->ops->value_get(node, ht->ctx);
}

/* An entry stands only as long as its value has a reference left. */
static bool lf_node_is_dead(struct lsv_ht *ht, struct lsv_lf_list_node *node)
{
	if (!ht->ops->value_is_dead)
		return false;

	return ht->ops->value_is_dead(node, ht->ctx);
}

/*
 * Harris delete, first half. Whoever wins this CAS owns the entry the cell
 * stood for and releases its reservation; the cell itself is unlinked and
 * retired by whichever traversal walks over it next.
 */
static bool lf_node_mark_deleted(struct lsv_ht *ht, struct lsv_lf_list_node *node,
				 struct lsv_lf_list_node *succ)
{
	if (lsv_ht_lcas(&node->next, succ, lsv_ht_mark_node(succ)) != succ)
		return false;

	atomic64_dec(&ht->entry_cnt);
	return true;
}

/*
 * A cell whose value has already lost its last reference stands for nothing:
 * whoever stumbles on it unpublishes it, so a remove that failed cannot leave
 * a key that can never be pinned and never be inserted again.
 */
static void lf_node_drop_dead(struct lsv_ht *ht, struct lsv_lf_list_node *node)
{
	struct lsv_lf_list_node *succ = READ_ONCE(node->next);

	if (lsv_ht_has_mark(succ))
		return;

	(void)lf_node_mark_deleted(ht, node, succ);
}

/*
 * A cell from node_alloc() given back unpublished: its value was made with an
 * initial reference for the caller the insertion would have returned it to.
 */
static void lf_node_discard_new(struct lsv_ht *ht, struct lsv_lf_list_node *node)
{
	if (ht->ops->value_put_new)
		ht->ops->value_put_new(node, ht->ctx);

	ht->ops->node_dealloc(node, ht->ctx);
}

/*
 * A key that is already there is pinned without allocating anything or
 * touching the entry count: the entry is reserved, and the cell taken, only
 * once the traversal has shown the key to be absent. Both survive a lost
 * insertion race, and both go back if the call ends up without linking it.
 *
 * |no_cell| reports a call that node_alloc() turned away.
 */
static void *lf_list_insert_or_inc(struct lsv_ht *ht, struct lsv_lf_list *list, u64 key,
				   bool *is_new, bool *no_cell, int tid)
{
	struct lsv_lf_list_node *new_node = NULL;
	void *value = NULL;
	u32 retry_cnt = 0;
	struct lsv_lf_list_node *right;
	struct lsv_lf_list_node *left;

	*is_new = false;
	*no_cell = false;

	for (;;) {
		right = lf_list_lookup(ht, list, key, &left, tid);
		if (!right)
			break;

		if (right != &ht->tail && right->key == key) {
			if (lf_node_ref_get(ht, right)) {
				value = right->value;
				break;
			}

			if (++retry_cnt > LSV_HT_MAX_LOOKUP_RETRIES)
				break;

			lf_node_drop_dead(ht, right);
			continue;
		}

		if (!new_node) {
			/*
			 * Reserved before the cell is linked, so that concurrent
			 * insertions cannot overshoot the size of the table.
			 */
			if (atomic64_inc_return(&ht->entry_cnt) > (s64)ht->max_entry_cnt) {
				atomic64_dec(&ht->entry_cnt);
				return NULL;
			}

			new_node = ht->ops->node_alloc(key, ht->ctx);
			if (!new_node) {
				atomic64_dec(&ht->entry_cnt);
				*no_cell = true;
				return NULL;
			}
		}

		/* |left| and |right| are protected by the region; the CAS validates them */
		new_node->next = right;
		ht_kunit_cell_linking(new_node);
		if (lsv_ht_lcas(&left->next, right, new_node) == right) {
			*is_new = true;
			return new_node->value;
		}

		if (++retry_cnt > LSV_HT_MAX_LOOKUP_RETRIES)
			break;
	}

	if (new_node) {
		atomic64_dec(&ht->entry_cnt);
		lf_node_discard_new(ht, new_node);
	}

	return value;
}

static struct lsv_lf_list_node *lf_list_insert(struct lsv_ht *ht, struct lsv_lf_list *list,
					       struct lsv_lf_list_node *new_node, u64 key,
					       int tid)
{
	u32 retry_cnt = 0;
	struct lsv_lf_list_node *right;
	struct lsv_lf_list_node *left;

	for (;;) {
		if (++retry_cnt > LSV_HT_MAX_LOOKUP_RETRIES)
			return NULL;

		right = lf_list_lookup(ht, list, key, &left, tid);
		if (!right)
			return NULL;

		if (right != &ht->tail && right->key == key)
			return right;

		new_node->next = right;
		if (lsv_ht_lcas(&left->next, right, new_node) == right)
			return new_node;
	}
}

/*
 * Harris delete: mark the cell, then let a lookup unlink it. Whoever unlinks
 * the cell retires it. A refcounted entry is removed only once its value has
 * no reference left, see lsv_ht_remove().
 */
static bool lf_list_remove(struct lsv_ht *ht, struct lsv_lf_list *list, u64 key, int tid)
{
	u32 retry_cnt = 0;
	struct lsv_lf_list_node *right_succ;
	struct lsv_lf_list_node *right;
	struct lsv_lf_list_node *left;

	for (;;) {
		if (++retry_cnt > LSV_HT_MAX_LOOKUP_RETRIES)
			return false;

		right = lf_list_lookup(ht, list, key, &left, tid);
		if (!right || right == &ht->tail || right->key != key)
			return false;

		if (ht->ops->value_is_dead && !lf_node_is_dead(ht, right))
			return false;

		right_succ = READ_ONCE(right->next);
		if (lsv_ht_has_mark(right_succ))
			return false;

		if (lf_node_mark_deleted(ht, right, right_succ)) {
			/* unlink it now, so that it is retired by this operation */
			(void)lf_list_lookup(ht, list, key, &left, tid);
			return true;
		}
	}
}

static void lsv_ht_bucket_cnt_set(struct lsv_ht *ht, u64 size, u64 load_factor)
{
	u64 raw_bucket_cnt = max_t(u64, size / load_factor, 1);
	u64 bits = __builtin_ctzll(roundup_pow_of_two(raw_bucket_cnt));

	bits = clamp_t(u64, bits, LSV_HT_MIN_BUCKET_BITS, LSV_HT_MAX_BUCKET_BITS);

	ht->bucket_bits = bits;
	ht->bucket_cnt = 1ULL << bits;
	ht->max_entry_cnt = size;
}

/*
 * The sentinels are the table's own: an empty bucket is a head whose next
 * pointer is the one tail, and neither ever reaches the ops.
 */
static int hash_ll_init(struct lsv_ht *ht)
{
	u64 i;

	ht->tail.key = LSV_HT_KEY_TAIL_SENTINEL;
	ht->tail.next = NULL;

	ht->buckets = vcalloc(ht->bucket_cnt, sizeof(*ht->buckets));
	if (!ht->buckets)
		return -ENOMEM;

	for (i = 0; i < ht->bucket_cnt; i++) {
		ht->buckets[i].head.key = LSV_HT_KEY_HEAD_SENTINEL;
		ht->buckets[i].head.next = &ht->tail;
	}

	return 0;
}

struct lsv_ht *lsv_ht_alloc_and_init(u64 size, void *ctx, const struct lsv_lf_list_ops *ops)
{
	struct lsv_ht *ht;
	int rc;

	/* references are counted by all three ops or by none */
	if (WARN_ON(!ops->value_get != !ops->value_is_dead ||
		    !ops->value_get != !ops->value_put_new))
		return ERR_PTR(-EINVAL);

	ht = kzalloc_obj(*ht);
	if (!ht)
		return ERR_PTR(-ENOMEM);

	ht->ctx = ctx;
	ht->ops = ops;
	lsv_ht_bucket_cnt_set(ht, size, LSV_HT_DEF_BUCKET_LOAD_FACTOR);

	ht->ebr = ebr_alloc(ht);
	if (!ht->ebr) {
		rc = -ENOMEM;
		goto err_free_ht;
	}

	rc = hash_ll_init(ht);
	if (rc)
		goto err_free_ebr;

	return ht;

err_free_ebr:
	ebr_destroy(ht->ebr);
err_free_ht:
	kfree(ht);
	return ERR_PTR(rc);
}

/*
 * The back-end may wait here for work it queued on the workers of other
 * participants, see ebr_limbo_destroy().
 */
void lsv_ht_free(struct lsv_ht *ht)
{
	u64 i;

	if (!ht)
		return;

	ebr_destroy(ht->ebr);

	for (i = 0; i < ht->bucket_cnt; i++)
		lf_list_free(ht, &ht->buckets[i]);

	vfree(ht->buckets);
	kfree(ht);
}

/* |freed| is what the end of the operation gave back. */
static void *ht_insert_or_inc_ref(struct lsv_ht *ht, u64 key, bool *is_new, bool *no_cell,
				  u32 *freed, int tid)
{
	void *value;

	ebr_enter(ht->ebr, tid);
	/* a full table still pins the keys it already holds */
	value = lf_list_insert_or_inc(ht, ht_bucket(ht, key), key, is_new, no_cell, tid);
	*freed = ebr_op_end(ht, tid, *no_cell);

	return value;
}

void *lsv_ht_insert_or_inc_ref(struct lsv_ht *ht, u64 key, bool *is_new)
{
	bool no_cell;
	void *value;
	u32 freed;
	int tid;

	*is_new = false;

	if (lsv_ht_key_is_reserved(key))
		return NULL;

	tid = ebr_tid(ht);
	if (tid < 0)
		return NULL;

	/*
	 * A cell the allocator lacked may have been among those the end of the
	 * operation has just given back: one more try then, and one only.
	 */
	value = ht_insert_or_inc_ref(ht, key, is_new, &no_cell, &freed, tid);
	if (!value && no_cell && freed)
		value = ht_insert_or_inc_ref(ht, key, is_new, &no_cell, &freed, tid);

	// i guess current tid is failed, reclaim others limbo bags
	if (!value && no_cell)
		ebr_request_reclaim(ht->ebr, tid);

	return value;
}

struct lsv_lf_list_node *lsv_ht_insert_node(struct lsv_ht *ht, u64 key,
					    struct lsv_lf_list_node *node)
{
	struct lsv_lf_list_node *cell;
	int tid;

	/* the list is sorted by node->key */
	if (lsv_ht_key_is_reserved(key) || WARN_ON_ONCE(node->key != key))
		return NULL;

	tid = ebr_tid(ht);
	if (tid < 0)
		return NULL;

	ebr_enter(ht->ebr, tid);
	cell = lf_list_insert(ht, ht_bucket(ht, key), node, key, tid);
	ebr_op_end(ht, tid, false);

	/* restore path: the count rebuilt from metadata needs no reservation */
	if (cell == node)
		atomic64_inc(&ht->entry_cnt);

	return cell;
}

void *lsv_ht_find_value(struct lsv_ht *ht, u64 key)
{
	struct lsv_lf_list_node *node;
	struct lsv_lf_list_node *left;
	void *value = NULL;
	int tid;

	tid = ebr_tid(ht);
	if (tid < 0)
		return NULL;

	ebr_enter(ht->ebr, tid);
	node = lf_list_lookup(ht, ht_bucket(ht, key), key, &left, tid);
	if (node && node != &ht->tail && node->key == key) {
		/*
		 * A value without a reference left is being torn down by its
		 * owner: hand it to nobody and unpublish it.
		 */
		if (lf_node_is_dead(ht, node))
			lf_node_drop_dead(ht, node);
		else
			value = node->value;
	}
	ebr_op_end(ht, tid, false);

	return value;
}

bool lsv_ht_remove(struct lsv_ht *ht, u64 key)
{
	bool removed;
	int tid;

	if (lsv_ht_key_is_reserved(key))
		return false;

	tid = ebr_tid(ht);
	if (tid < 0)
		return false;

	ebr_enter(ht->ebr, tid);
	removed = lf_list_remove(ht, ht_bucket(ht, key), key, tid);
	ebr_op_end(ht, tid, false);

	return removed;
}

u64 lsv_ht_epoch(const struct lsv_ht *ht)
{
	return ebr_epoch(ht->ebr);
}

/* An insertion under way counts: its entry is reserved before it is linked. */
bool lsv_ht_is_empty(struct lsv_ht *ht)
{
	return !lsv_ht_entry_cnt(ht);
}

/* |node_size| is a cell of the ops, and the table itself takes none. */
size_t lsv_ht_mem_usage(struct lsv_ht *ht, size_t node_size)
{
	size_t bytes = sizeof(*ht);

	bytes += ht->bucket_cnt * sizeof(*ht->buckets);
	bytes += ebr_mem_usage(ht->ebr);

	return bytes;
}
