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


/* one cell for every entry of the table: a byte here is a byte per entry */
static_assert(sizeof(struct lsv_lf_list_node) == 40);
/* and one bucket for every LSV_HT_DEF_BUCKET_LOAD_FACTOR of them */
static_assert(sizeof(struct lsv_lf_list) == sizeof(struct lsv_lf_list_node));

/* An insertion is about to link its cell; KUnit stalls it here. */
VISIBLE_IF_KUNIT void ht_kunit_cell_linking(struct lsv_lf_list_node *cell)
{
	KUNIT_STATIC_STUB_REDIRECT(ht_kunit_cell_linking, cell);
}

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
 * Harris find ([DtA] Listing 2 without its anchors). Returns the first
 * cell whose key is not below |key| and its live predecessor, after
 * physically unlinking the deleted cells between them. The unlinking thread
 * retires the cells it bypassed.
 */
static struct lsv_lf_list_node *lf_list_lookup(struct lsv_lf_list *list, u64 key,
					       struct lsv_lf_list_node **left_out,
					       struct lsv_ebr_state *ebr, int tid)
{
	struct lsv_lf_list_node *tail = ebr->tail;
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

		t = lsv_ht_strip_all(t_next);
		if (t == tail)
			break;
		ht_kunit_cell_reached(t);
		t_next = READ_ONCE(t->next);
	}

	right = t;

	if (left_next_snap == right) {
		// why
		if (right != tail && lsv_ht_has_mark(READ_ONCE(right->next)))
			goto retry;
		return right;
	}

	// what is that case?
	if (lsv_ht_lcas(&(*left_out)->next, left_next_snap, right) == left_next_snap)
		ebr_retire_span(ebr, tid, left_next_snap, right);

	goto retry;
}

static bool lf_node_ref_get(struct lsv_ebr_state *ebr, struct lsv_lf_list_node *node)
{
	if (!ebr->ops->value_get)
		return true;

	return ebr->ops->value_get(node, ebr->ctx);
}

/* Harris logical deletion */
static bool lf_node_mark_deleted(struct lsv_ebr_state *ebr, struct lsv_lf_list_node *node,
				 struct lsv_lf_list_node *succ)
{
	if (lsv_ht_lcas(&node->next, succ, lsv_ht_mark_node(succ)) != succ)
		return false;

	atomic64_dec(&ebr->ht->entry_cnt);

	return true;
}

/* An entry stands only as long as its value has a reference left. */
static bool lf_node_is_dead(struct lsv_ebr_state *ebr, struct lsv_lf_list_node *node)
{
	if (!ebr->ops->value_is_dead)
		return false;

	return ebr->ops->value_is_dead(node, ebr->ctx);
}

static void lf_node_drop_dead(struct lsv_lf_list_node *node, struct lsv_ebr_state *ebr, int tid)
{
	struct lsv_lf_list_node *succ = READ_ONCE(node->next);

	if (lsv_ht_has_mark(succ))
		return;

	(void)lf_node_mark_deleted(ebr, node, succ);
}

static void lf_node_discard_new(struct lsv_ebr_state *ebr, struct lsv_lf_list_node *node)
{
	if (ebr->ops->value_put_new)
		ebr->ops->value_put_new(node, ebr->ctx);

	ebr->ops->node_dealloc(node, ebr->ctx);
}

/*
 * A key that is already there is pinned without touching the entry count,
 * which every bucket shares. An absent one needs |cell|: without it the call
 * links nothing and says so in |need_cell|, for the caller to make one outside
 * the operation, since node_alloc() may sleep. |cell| stays the caller's
 * unless the call reports it linked in |is_new|.
 */
static void *lf_list_insert_or_inc(struct lsv_lf_list *list, u64 key,
				   struct lsv_lf_list_node *cell, bool *is_new,
				   bool *need_cell, struct lsv_ebr_state *ebr, int tid)
{
	u32 retry_cnt = 0;
	struct lsv_lf_list_node *right;
	struct lsv_lf_list_node *left;

	*is_new = false;
	*need_cell = false;

	for (;;) {
		right = lf_list_lookup(list, key, &left, ebr, tid);
		if (!right)
			return NULL;

		if (right != ebr->tail && right->key == key) {
			if (lf_node_ref_get(ebr, right))
				return right->value;

			if (++retry_cnt > LSV_HT_MAX_LOOKUP_RETRIES)
				return NULL;

			lf_node_drop_dead(right, ebr, tid);
			continue;
		}

		if (!cell) {
			*need_cell = true;
			return NULL;
		}

		cell->next = right;
		ht_kunit_cell_linking(cell);
		if (lsv_ht_lcas(&left->next, right, cell) == right) {
			*is_new = true;
			return cell->value;
		}

		/* a lost link costs the budget like any other retry of the call */
		if (++retry_cnt > LSV_HT_MAX_LOOKUP_RETRIES)
			return NULL;
	}
}

static struct lsv_lf_list_node *lf_list_insert(struct lsv_lf_list *list,
					       struct lsv_lf_list_node *new_node, u64 key,
					       struct lsv_ebr_state *ebr, int tid)
{
	u32 retry_cnt = 0;
	struct lsv_lf_list_node *right;
	struct lsv_lf_list_node *left;

	for (;;) {
		if (++retry_cnt > LSV_HT_MAX_LOOKUP_RETRIES)
			return NULL;

		right = lf_list_lookup(list, key, &left, ebr, tid);
		if (!right)
			return NULL;

		if (right != ebr->tail && right->key == key)
			return right;

		new_node->next = right;
		ht_kunit_cell_linking(new_node);
		if (lsv_ht_lcas(&left->next, right, new_node) == right)
			return new_node;
	}
}

/*
 * Harris delete: mark the cell, then let a lookup unlink it. Whoever unlinks
 * the cell retires it.
 *
 * A refcounted entry is removed only once its value has no reference left.
 */
static bool lf_list_remove(struct lsv_lf_list *list, u64 key,
			   struct lsv_ebr_state *ebr, int tid)
{
	u32 retry_cnt = 0;
	struct lsv_lf_list_node *right_succ;
	struct lsv_lf_list_node *right;
	struct lsv_lf_list_node *left;

	for (;;) {
		if (++retry_cnt > LSV_HT_MAX_LOOKUP_RETRIES)
			return false;

		right = lf_list_lookup(list, key, &left, ebr, tid);
		if (!right || right == ebr->tail || right->key != key)
			return false;

		if (ebr->ops->value_is_dead && !lf_node_is_dead(ebr, right))
			return false;

		right_succ = READ_ONCE(right->next);

		if (lsv_ht_has_mark(right_succ))
			return false;

		if (lf_node_mark_deleted(ebr, right, right_succ)) {
			(void)lf_list_lookup(list, key, &left, ebr, tid);
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
	ebr_free(ht->ebr, false);
err_free_ht:
	kfree(ht);
	return ERR_PTR(rc);
}

/*
 * Whether a reclaim request is queued on the worker this runs on. It cannot
 * be running: the worker is busy here, and runs it only after this returns.
 */
static bool ebr_own_request_queued(struct lsv_ebr_state *ebr)
{
	struct lsv_worker *worker = lsv_wq_current_worker();
	unsigned int tid;

	if (!worker)
		return false;

	tid = lsv_worker_id(worker);
	return tid < ebr->nr_threads && atomic_read(&ebr->local[tid].reclaim_queued);
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
	own_request = ebr_own_request_queued(ht->ebr);
	/* the acquire pairs with the release of ebr_reclaim_request_run() */
	while (atomic_read_acquire(&ht->ebr->reclaim_pending) > own_request)
		cond_resched();

	ebr_free(ht->ebr, own_request);

	for (i = 0; i < ht->bucket_cnt; i++)
		lf_list_free(ht, &ht->buckets[i]);

	vfree(ht->buckets);
	kfree(ht);
}

static void *ht_insert_or_inc_ref(struct lsv_ht *ht, u64 key, struct lsv_lf_list_node *cell,
				  bool *is_new, bool *need_cell, int tid)
{
	void *value;

	ebr_begin_op(ht->ebr, tid);
	/* a full table still pins the keys it already holds */
	value = lf_list_insert_or_inc(ht_bucket(ht, key), key, cell, is_new, need_cell,
				      ht->ebr, tid);
	ht_op_end(ht, tid);

	return value;
}

/*
 * A cell for |key|, made between two operations. The entry is reserved first,
 * so that concurrent insertions cannot overshoot the size of the table, and
 * goes back with the cell if the insertion does not link it. |no_cell|
 * reports a call that node_alloc() turned away, not a full table.
 */
static struct lsv_lf_list_node *ht_cell_new(struct lsv_ht *ht, u64 key, bool *no_cell)
{
	struct lsv_lf_list_node *cell;

	*no_cell = false;

	if (atomic64_inc_return(&ht->entry_cnt) > (s64)ht->max_entry_cnt) {
		atomic64_dec(&ht->entry_cnt);
		return NULL;
	}

	cell = ht->ops->node_alloc(key, ht->ctx);
	if (!cell) {
		atomic64_dec(&ht->entry_cnt);
		*no_cell = true;
	}

	return cell;
}

/*
 * The first operation only looks: a key that is there is pinned at once. An
 * absent one gets a cell made outside any operation, and a second operation
 * looks again, since the key may have come in meanwhile, and links the cell.
 * At most one cell is taken per call.
 */
void *lsv_ht_insert_or_inc_ref(struct lsv_ht *ht, u64 key, bool *is_new)
{
	struct lsv_lf_list_node *cell;
	bool need_cell;
	bool no_cell;
	void *value;
	int tid;

	*is_new = false;

	if (lsv_ht_key_is_reserved(key))
		return NULL;

	tid = ht_tid(ht);
	if (tid < 0)
		return NULL;

	value = ht_insert_or_inc_ref(ht, key, NULL, is_new, &need_cell, tid);
	if (value || !need_cell)
		return value;

	/*
	 * A cell the allocator lacked may be among those this participant
	 * has retired: one more try once its buffer is scanned, and one only.
	 */
	cell = ht_cell_new(ht, key, &no_cell);
	if (!cell && no_cell && ebr_try_reclaim(ht->ebr, tid))
		cell = ht_cell_new(ht, key, &no_cell);

	if (!cell) {
		/*
		 * What this participant could not get back itself may sit in
		 * the buffers of others, some of which may never operate on
		 * the table again: have them scan their buffers, for the
		 * insertions to come.
		 */
		if (no_cell)
			ebr_request_reclaim(ht->ebr, tid);
		return NULL;
	}

	value = ht_insert_or_inc_ref(ht, key, cell, is_new, &need_cell, tid);
	if (!*is_new) {
		/* the key came in meanwhile, or the call gave up */
		atomic64_dec(&ht->entry_cnt);
		lf_node_discard_new(ht->ebr, cell);
	}

	return value;
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

	ebr_begin_op(ht->ebr, tid);
	list = ht_bucket(ht, key);
	node = lf_list_lookup(list, key, &left, ht->ebr, tid);
	if (node && node->key == key) {
		/*
		 * A value without a reference left is being torn down by its
		 * owner: the entry is gone, whether or not the removal that
		 * says so has run yet. Hand it to nobody and unpublish it.
		 */
		if (lf_node_is_dead(ht->ebr, node))
			lf_node_drop_dead(node, ht->ebr, tid);
		else
			value = node->value;
	}
	ht_op_end(ht, tid);

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

	ebr_begin_op(ht->ebr, tid);
	removed = lf_list_remove(ht_bucket(ht, key), key, ht->ebr, tid);
	ht_op_end(ht, tid);

	return removed;
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
	bytes += sizeof(*ht->ebr);
	bytes += ht->ebr->nr_threads * (sizeof(*ht->ebr->global) + sizeof(*ht->ebr->local));

	return bytes;
}
