#include <linux/err.h>
#include <linux/log2.h>
#include <linux/minmax.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <kunit/static_stub.h>
#include <kunit/visibility.h>

#include "dta.h"
#include "tests/include_static/__static_kunit_lsv_ht.h"


/* one cell for every entry of the table: a byte here is a byte per entry */
static_assert(sizeof(struct lsv_lf_list_node) == 48);
/* and one bucket for every LSV_HT_DEF_BUCKET_LOAD_FACTOR of them */
static_assert(sizeof(struct lsv_lf_list) == sizeof(struct lsv_lf_list_node));

/* A traversal holds a mid-list anchor; KUnit stalls a participant here. */
VISIBLE_IF_KUNIT void ht_anchor_dropped(struct lsv_lf_list_node *anchor)
{
	KUNIT_STATIC_STUB_REDIRECT(ht_anchor_dropped, anchor);
}

/* An insertion is about to link its cell; KUnit stalls it here. */
VISIBLE_IF_KUNIT void ht_cell_linking(struct lsv_lf_list_node *cell)
{
	KUNIT_STATIC_STUB_REDIRECT(ht_cell_linking, cell);
}

/*
 * Cells still linked were never retired, whatever their tags, so every cell
 * reachable from the head is freed here; retired ones are freed by dta_free().
 */
static void lf_list_free(struct lsv_ht *ht, struct lsv_lf_list *list)
{
	struct lsv_lf_list_node *node;
	struct lsv_lf_list_node *next;

	for (node = lsv_ht_strip_all(list->head.next); node != &ht->tail; node = next) {
		next = lsv_ht_strip_all(node->next);
		ht->ops->node_dealloc(node, ht->ctx);
	}
}

/*
 * Harris find with [DtA] anchor maintenance (Listing 2). Returns the first
 * cell whose key is not below |key| and its live predecessor, after
 * physically unlinking the deleted cells between them. The unlinking thread
 * retires the cells it bypassed.
 */
static struct lsv_lf_list_node *lf_list_lookup(struct lsv_lf_list *list, u64 key,
					       struct lsv_lf_list_node **left_out,
					       struct lsv_dta_state *dta, int tid)
{
	struct lsv_lf_list_node *tail = dta->tail;
	u32 retry_count = 0;
	struct lsv_lf_list_node *left_next_snap;
	struct lsv_lf_list_node *t_next;
	struct lsv_lf_list_node *right;
	struct lsv_lf_list_node *t;
	u32 nodes_after_anchor;

	if (lsv_ht_key_is_reserved(key))
		return NULL;

retry:
	if (++retry_count > LSV_HT_MAX_LOOKUP_RETRIES)
		return NULL;

	/* the restart point is the only place a recovered thread may resume */
	if (!dta_set_anchor(dta, tid, &list->head)) {
		dta_help_recovery(dta, tid);
		goto retry;
	}

	nodes_after_anchor = 0;
	*left_out = &list->head;
	t = &list->head;
	/* one load per cell: every decision below must use the same value */
	t_next = READ_ONCE(t->next);
	left_next_snap = t_next;

	while (lsv_ht_has_mark(t_next) || (t != tail && t->key < key)) {
		if (!lsv_ht_has_mark(t_next)) {
			*left_out = t;
			left_next_snap = t_next;

			if (++nodes_after_anchor % LSV_DTA_ANCHOR_THRESHOLD == 0) {
				if (!dta_set_anchor(dta, tid, t)) {
					dta_help_recovery(dta, tid);
					goto retry;
				}
				ht_anchor_dropped(t);
			}
		}

		t = lsv_ht_strip_all(t_next);
		if (t == tail)
			break;
		ht_cell_reached(t);
		t_next = READ_ONCE(t->next);
	}

	right = t;

	if (left_next_snap == right) {
		if (right != tail && lsv_ht_has_mark(READ_ONCE(right->next)))
			goto retry;
		return right;
	}

	/* a frozen predecessor must not be updated: help the recovery first */
	if (lsv_ht_is_frozen(left_next_snap)) {
		dta_help_recovery(dta, tid);
		goto retry;
	}

	if (lsv_ht_lcas(&(*left_out)->next, left_next_snap, right) == left_next_snap)
		dta_retire_span(dta, tid, left_next_snap, right);

	goto retry;
}

/*
 * A value's reference count and its presence in the list change separately:
 * the last put drops the count to zero before the key is removed, so a zero
 * count means the value is being torn down and must not be resurrected.
 */
static bool lf_node_ref_get(struct lsv_dta_state *dta, struct lsv_lf_list_node *node)
{
	if (!dta->ops->value_get)
		return true;

	return dta->ops->value_get(node, dta->ctx);
}

/*
 * Harris delete, first half. Whoever wins this CAS owns the entry the cell
 * stood for and releases its reservation; the cell itself is unlinked and
 * retired by whichever traversal walks over it next.
 */
static bool lf_node_mark_deleted(struct lsv_dta_state *dta, struct lsv_lf_list_node *node,
				 struct lsv_lf_list_node *succ)
{
	if (lsv_ht_lcas(&node->next, succ, lsv_ht_mark_node(succ)) != succ)
		return false;

	atomic64_dec(&dta->ht->entry_cnt);
	return true;
}

/*
 * A cell whose value has already lost its last reference stands for nothing:
 * its owner's lsv_ht_remove() is either still on the way or has given up.
 * Whoever stumbles on such a cell unpublishes it, so a remove that failed
 * cannot leave a key that can never be pinned and never be inserted again.
 */
/* An entry stands only as long as its value has a reference left. */
static bool lf_node_is_dead(struct lsv_dta_state *dta, struct lsv_lf_list_node *node)
{
	if (!dta->ops->value_is_dead)
		return false;

	return dta->ops->value_is_dead(node, dta->ctx);
}

static void lf_node_drop_dead(struct lsv_lf_list_node *node, struct lsv_dta_state *dta, int tid)
{
	struct lsv_lf_list_node *succ = READ_ONCE(node->next);

	if (lsv_ht_has_mark(succ))
		return;

	/* frozen cells cannot be marked deleted ([DtA] Lemma 4.3) */
	if (lsv_ht_is_frozen(succ)) {
		dta_help_recovery(dta, tid);
		return;
	}

	(void)lf_node_mark_deleted(dta, node, succ);
}

/*
 * A cell from node_alloc() that is given back unpublished: its value was made
 * with an initial reference for the caller the insertion would have returned
 * it to, and nobody else has seen it. node_dealloc() releases no reference, so
 * the table drops that one itself before giving the cell back.
 */
static void lf_node_discard_new(struct lsv_dta_state *dta, struct lsv_lf_list_node *node)
{
	if (dta->ops->value_put_new)
		dta->ops->value_put_new(node, dta->ctx);

	dta->ops->node_dealloc(node, dta->ctx);
}

/*
 * A key that is already there is pinned without allocating anything or
 * touching the entry count, which every bucket shares: the entry is reserved,
 * and the cell taken, only once the traversal has shown the key to be absent.
 * Both survive a lost insertion race, so at most one cell is taken per call,
 * and both go back if the call ends up without linking it.
 *
 * |no_cell| reports a call that node_alloc() turned away.
 */
static void *lf_list_insert_or_inc(struct lsv_lf_list *list, u64 key, bool *is_new,
				   bool *no_cell, struct lsv_dta_state *dta, int tid)
{
	struct lsv_lf_list_node *new_node = NULL;
	struct lsv_ht *ht = dta->ht;
	void *value = NULL;
	u32 retry_cnt = 0;
	struct lsv_lf_list_node *right;
	struct lsv_lf_list_node *left;

	*is_new = false;
	*no_cell = false;

	for (;;) {
		right = lf_list_lookup(list, key, &left, dta, tid);
		if (!right)
			break;

		if (right != dta->tail && right->key == key) {
			if (lf_node_ref_get(dta, right)) {
				value = right->value;
				break;
			}

			if (++retry_cnt > LSV_HT_MAX_LOOKUP_RETRIES)
				break;

			lf_node_drop_dead(right, dta, tid);
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

			new_node = dta->ops->node_alloc(key, dta->ctx);
			if (!new_node) {
				atomic64_dec(&ht->entry_cnt);
				*no_cell = true;
				return NULL;
			}
		}

		/*
		 * |left| and |right| may be stale after the allocation, but the
		 * operation still protects them and the CAS validates them.
		 */
		new_node->next = right;
		/*
		 * A cell is as old as its link, not as the insertion that made
		 * it: one stamped earlier may land behind a traversal that began
		 * in between, which a recovery would then count as a cell that
		 * traversal passed. Until it is stamped, it counts as new.
		 */
		new_node->insert_ts = U64_MAX;
		ht_cell_linking(new_node);
		if (lsv_ht_lcas(&left->next, right, new_node) == right) {
			WRITE_ONCE(new_node->insert_ts, dta_calc_max_ts(dta) + 1);
			*is_new = true;
			return new_node->value;
		}

		/* a lost link costs the budget like any other retry of the call */
		if (++retry_cnt > LSV_HT_MAX_LOOKUP_RETRIES)
			break;
	}

	if (new_node) {
		atomic64_dec(&ht->entry_cnt);
		lf_node_discard_new(dta, new_node);
	}

	return value;
}

static struct lsv_lf_list_node *lf_list_insert(struct lsv_lf_list *list,
					       struct lsv_lf_list_node *new_node, u64 key,
					       struct lsv_dta_state *dta, int tid)
{
	u32 retry_cnt = 0;
	struct lsv_lf_list_node *right;
	struct lsv_lf_list_node *left;

	for (;;) {
		if (++retry_cnt > LSV_HT_MAX_LOOKUP_RETRIES)
			return NULL;

		right = lf_list_lookup(list, key, &left, dta, tid);
		if (!right)
			return NULL;

		if (right != dta->tail && right->key == key)
			return right;

		new_node->next = right;
		new_node->insert_ts = U64_MAX;
		ht_cell_linking(new_node);
		if (lsv_ht_lcas(&left->next, right, new_node) == right) {
			WRITE_ONCE(new_node->insert_ts, dta_calc_max_ts(dta) + 1);
			return new_node;
		}
	}
}

/*
 * Harris delete: mark the cell, then let a lookup unlink it. Whoever unlinks
 * the cell retires it.
 *
 * A refcounted entry is removed only once its value has no reference left.
 * The remove that follows a last put may come late: by then the dead entry
 * can have been dropped by another operation and the key inserted anew, and
 * the key alone would then take the new, live entry out of the table. A zero
 * count never rises again, so the check cannot be overtaken.
 */
static bool lf_list_remove(struct lsv_lf_list *list, u64 key,
			   struct lsv_dta_state *dta, int tid)
{
	u32 retry_cnt = 0;
	struct lsv_lf_list_node *right_succ;
	struct lsv_lf_list_node *right;
	struct lsv_lf_list_node *left;

	for (;;) {
		if (++retry_cnt > LSV_HT_MAX_LOOKUP_RETRIES)
			return false;

		right = lf_list_lookup(list, key, &left, dta, tid);
		if (!right || right == dta->tail || right->key != key)
			return false;

		if (dta->ops->value_is_dead && !lf_node_is_dead(dta, right))
			return false;

		right_succ = READ_ONCE(right->next);

		if (lsv_ht_has_mark(right_succ))
			return false;

		/* frozen cells cannot be marked deleted ([DtA] Lemma 4.3) */
		if (lsv_ht_is_frozen(right_succ)) {
			dta_help_recovery(dta, tid);
			continue;
		}

		if (lf_node_mark_deleted(dta, right, right_succ)) {
			(void)lf_list_lookup(list, key, &left, dta, tid);
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

	if (!system_has_cmpxchg128())
		return ERR_PTR(-EOPNOTSUPP);

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

	ht->dta = dta_alloc(ht);
	if (!ht->dta) {
		rc = -ENOMEM;
		goto err_free_ht;
	}

	rc = hash_ll_init(ht);
	if (rc)
		goto err_free_dta;

	return ht;

err_free_dta:
	dta_free(ht->dta, false);
err_free_ht:
	kfree(ht);
	return ERR_PTR(rc);
}

/*
 * Whether a reclaim request is queued on the worker this runs on. It cannot
 * be running: the worker is busy here, and runs it only after this returns.
 */
static bool dta_own_request_queued(struct lsv_dta_state *dta)
{
	struct lsv_worker *worker = lsv_wq_current_worker();
	unsigned int tid;

	if (!worker)
		return false;

	tid = lsv_worker_id(worker);
	return tid < dta->nr_threads && atomic_read(&dta->local[tid].reclaim_queued);
}

void lsv_ht_free(struct lsv_ht *ht)
{
	bool own_request;
	u64 i;

	if (!ht)
		return;

	/*
	 * Reclaim requests still queued name the table, and no operation is
	 * left to queue another one. Those of the other workers are waited
	 * for; the one queued behind this very call would never come, so it
	 * is left the memory it touches, and frees it once it runs.
	 */
	own_request = dta_own_request_queued(ht->dta);
	while (atomic_read(&ht->dta->reclaim_pending) > own_request)
		cond_resched();

	dta_free(ht->dta, own_request);

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

	dta_begin_op(ht->dta, tid, &ht_bucket(ht, key)->head);

	/* a full table still pins the keys it already holds */
	value = lf_list_insert_or_inc(ht_bucket(ht, key), key, is_new, no_cell, ht->dta, tid);

	/* the cells this very operation may be missing can come back only here */
	*freed = ht_op_end(ht, tid, *no_cell);

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

	tid = ht_tid(ht);
	if (tid < 0)
		return NULL;

	/*
	 * A cell the allocator lacked may have been among those the end of the
	 * operation has just given back: one more try then, and one only.
	 */
	value = ht_insert_or_inc_ref(ht, key, is_new, &no_cell, &freed, tid);
	if (!value && no_cell && freed)
		value = ht_insert_or_inc_ref(ht, key, is_new, &no_cell, &freed, tid);

	/*
	 * What this participant could not get back itself may sit in the
	 * buffers of others, some of which may never operate on the table
	 * again: have them scan their buffers, for the insertions to come.
	 */
	if (!value && no_cell)
		dta_request_reclaim(ht->dta, tid);

	return value;
}

struct lsv_lf_list_node *lsv_ht_insert_node(struct lsv_ht *ht, u64 key,
					    struct lsv_lf_list_node *node)
{
	struct lsv_lf_list_node *cell;
	int tid;

	/* the list is sorted, and a recovery finds a cell's bucket, by node->key */
	if (lsv_ht_key_is_reserved(key) || WARN_ON_ONCE(node->key != key))
		return NULL;

	tid = ht_tid(ht);
	if (tid < 0)
		return NULL;

	dta_begin_op(ht->dta, tid, &ht_bucket(ht, key)->head);
	cell = lf_list_insert(ht_bucket(ht, key), node, key, ht->dta, tid);
	ht_op_end(ht, tid, false);

	/* restore path: the count rebuilt from metadata needs no reservation */
	if (cell == node)
		atomic64_inc(&ht->entry_cnt);

	return cell;
}

void *lsv_ht_find_value(struct lsv_ht *ht, u64 key)
{
	void *value = NULL;
	struct lsv_lf_list_node *node;
	struct lsv_lf_list_node *left;
	struct lsv_lf_list *list;
	int tid;

	tid = ht_tid(ht);
	if (tid < 0)
		return NULL;

	dta_begin_op(ht->dta, tid, &ht_bucket(ht, key)->head);
	list = ht_bucket(ht, key);
	node = lf_list_lookup(list, key, &left, ht->dta, tid);
	if (node && node->key == key) {
		/*
		 * A value without a reference left is being torn down by its
		 * owner: the entry is gone, whether or not the removal that
		 * says so has run yet. Hand it to nobody and unpublish it.
		 */
		if (lf_node_is_dead(ht->dta, node))
			lf_node_drop_dead(node, ht->dta, tid);
		else
			value = node->value;
	}
	ht_op_end(ht, tid, false);

	return value;
}

bool lsv_ht_remove(struct lsv_ht *ht, u64 key)
{
	bool removed;
	int tid;

	if (lsv_ht_key_is_reserved(key))
		return false;

	tid = ht_tid(ht);
	if (tid < 0)
		return false;

	dta_begin_op(ht->dta, tid, &ht_bucket(ht, key)->head);
	removed = lf_list_remove(ht_bucket(ht, key), key, ht->dta, tid);
	ht_op_end(ht, tid, false);

	return removed;
}

u64 lsv_ht_recovery_cnt(const struct lsv_ht *ht)
{
	return atomic64_read(&ht->dta->recovery_cnt);
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
	bytes += sizeof(*ht->dta);
	bytes += ht->dta->nr_threads * (sizeof(*ht->dta->global) + sizeof(*ht->dta->local));

	return bytes;
}
