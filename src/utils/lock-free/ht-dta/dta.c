#include <linux/build_bug.h>
#include <linux/printk.h>
#include <kunit/static_stub.h>
#include <kunit/visibility.h>

#include "dta.h"
#include "tests/include_static/__static_kunit_lsv_ht.h"

enum lsv_dta_find_rc {
	LSV_DTA_FIND_FOUND,
	LSV_DTA_FIND_DONE,
	LSV_DTA_FIND_ABORT,
	LSV_DTA_FIND_CNT,
};

static __always_inline bool dta_dwcas(struct lsv_dta_global_rec *rec,
					   u64 old_ts, u64 old_af,
					   u64 new_ts, u64 new_af)
{
	u128 old_val = ((u128)old_af << 64) | (u128)old_ts;
	u128 new_val = ((u128)new_af << 64) | (u128)new_ts;

	return cmpxchg128((u128 *)rec, old_val, new_val) == old_val;
}

static void dta_recovery_enter(struct lsv_dta_state *dta)
{
	atomic_inc(&dta->recovery_active);
	atomic64_inc(&dta->recovery_seq);
}

static void dta_recovery_exit(struct lsv_dta_state *dta)
{
	atomic64_inc(&dta->recovery_seq);
	atomic_dec(&dta->recovery_active);
}

u64 dta_calc_max_ts(const struct lsv_dta_state *dta)
{
	u64 max_ts = 0;
	unsigned int i;
	u64 ts;

	for (i = 0; i < dta->nr_threads; i++) {
		ts = READ_ONCE(dta->global[i].timestamp);
		if (ts > max_ts)
			max_ts = ts;
	}

	return max_ts;
}

/*
 * [DtA] 4.1: start an operation with a timestamp above every other one.
 *
 * |anchor| is where the operation's first traversal starts from, the head of
 * its bucket, or NULL for an operation that restarts before it knows. Taking
 * it in the same CAS as the timestamp spares the traversal one of its own.
 */
/*
 * An operation has computed its timestamp and not published it yet; KUnit
 * stalls it here. The timestamp published afterwards may be below that of a
 * participant that began in between: nothing but the record CAS validates it.
 */
VISIBLE_IF_KUNIT void ht_begin_computed(unsigned int tid)
{
	KUNIT_STATIC_STUB_REDIRECT(ht_begin_computed, tid);
}

void dta_begin_op(struct lsv_dta_state *dta, int tid, struct lsv_lf_list_node *anchor)
{
	struct lsv_dta_global_rec *rec = &dta->global[tid];
	u64 old_ts;
	u64 old_af;
	u64 new_ts;
	u64 new_af;

	do {
		old_ts = READ_ONCE(rec->timestamp);
		old_af = READ_ONCE(rec->anchor_and_flags);

		/* A recovery owns the record; the first anchor drop notices it. */
		if (lsv_dta_af_is_suspected(old_af))
			return;

		new_ts = dta_calc_max_ts(dta) + 1;
		WRITE_ONCE(rec->low_timestamp, new_ts - 1);
		/* no anchor survives from the last operation: see dta_end_op() */
		new_af = lsv_dta_af_make(anchor, false, false);
		ht_begin_computed(tid);
	} while (!dta_dwcas(rec, old_ts, old_af, new_ts, new_af));
}

static void dta_recover(struct lsv_dta_state *dta, int stuck_tid, int my_tid,
			u64 old_ts, u64 old_af);

/*
 * [DtA] 4.1: finishing an operation turns the IDLE bit on and drops the
 * anchor. A suspected thread completes its own recovery first: helpers only
 * look for suspected threads, so an idle bit set on top of a half-done
 * recovery would leave its frozen range without an owner.
 *
 * The anchor must not outlive the operation. An idle thread is left out of the
 * reclamation bounds, so the cell its anchor names is no longer protected and
 * may be freed; a stale pointer left in the record would be dereferenced by a
 * recovery that catches the record just as it goes idle, or by one that
 * suspects the next operation before it has published an anchor of its own.
 */
static void dta_end_op(struct lsv_dta_state *dta, int tid)
{
	struct lsv_dta_global_rec *rec = &dta->global[tid];
	bool helped = false;
	u64 old_ts;
	u64 old_af;
	u64 new_af;

	for (;;) {
		old_ts = READ_ONCE(rec->timestamp);
		old_af = READ_ONCE(rec->anchor_and_flags);

		if (lsv_dta_af_is_suspected(old_af)) {
			/* still suspected: a later helper or operation finishes it */
			if (helped)
				return;
			dta_recover(dta, tid, tid, old_ts, old_af);
			helped = true;
			continue;
		}

		/*
		 * A participant that gets here has finished its traversals and
		 * holds nothing any more, so a recovery that ended on it needs
		 * no marker either: left on, the stuck bit would keep its
		 * recovery timestamp in the reclamation bounds, and the cells
		 * put aside for it out of reach, until it runs again, which on
		 * an unevenly loaded pool may be a long time.
		 */
		new_af = LSV_DTA_IDLE_BIT;
		if (dta_dwcas(rec, old_ts, old_af, old_ts, new_af))
			return;
	}
}

/*
 * [DtA] Listing 3. Fails once the STUCK bit is set: a stuck thread must help
 * its own recovery, and a recovered one may only return to running by
 * restarting its operation (Figure 1), never in the middle of a traversal.
 */
bool dta_set_anchor(struct lsv_dta_state *dta, int tid,
			   struct lsv_lf_list_node *new_anchor)
{
	struct lsv_dta_global_rec *rec = &dta->global[tid];
	u64 old_ts;
	u64 old_af;
	u64 new_af;

	old_ts = READ_ONCE(rec->timestamp);
	old_af = READ_ONCE(rec->anchor_and_flags);

	if (lsv_dta_af_is_stuck(old_af))
		return false;

	/* the record reads as this anchor already, the first one of an op included */
	new_af = lsv_dta_af_make(new_anchor, false, false);
	if (old_af == new_af)
		return true;

	return dta_dwcas(rec, old_ts, old_af, old_ts, new_af);
}

/* Reclamation has read the flags of a record, not yet its timestamp again; KUnit stalls it here. */
VISIBLE_IF_KUNIT void ht_record_flags_read(unsigned int tid)
{
	KUNIT_STATIC_STUB_REDIRECT(ht_record_flags_read, tid);
}

#if IS_ENABLED(CONFIG_KUNIT)
/*
 * Called on every cell any walk steps on, from any participant: KUnit checks
 * there that no walk ever reaches a cell the table has given back. A plain
 * pointer and not a static stub, whose lookup would serialize the walks of a
 * stress test on the lock of the test.
 */
void (*g_lsv_ht_cell_check)(struct lsv_lf_list_node *cell);
#endif

/*
 * A walk has stepped onto a cell and is about to read its next pointer --
 * a traversal, or a recovery freezing, copying or splicing a range; KUnit
 * checks the cell here, and stalls a participant anywhere between two anchors.
 */
void ht_cell_reached(struct lsv_lf_list_node *cell)
{
#if IS_ENABLED(CONFIG_KUNIT)
	void (*check)(struct lsv_lf_list_node *cell) = READ_ONCE(g_lsv_ht_cell_check);

	if (check)
		check(cell);
#endif
	KUNIT_STATIC_STUB_REDIRECT(ht_cell_reached, cell);
}

/* A walk of a recovery has stepped onto |cell|. */
static void dta_recovery_step(struct lsv_dta_state *dta, struct lsv_lf_list_node *cell)
{
#if IS_ENABLED(CONFIG_KUNIT)
	atomic64_inc(&dta->recovery_walked);
#endif
	ht_cell_reached(cell);
}

/*
 * The timestamp and the flags of a record as one state. The two words change
 * together only by double-width CAS, and a timestamp never comes back: every
 * write of it is dta_calc_max_ts() + 1, above the one it replaces. So flags
 * read between two equal loads of the timestamp belong to that timestamp.
 *
 * Two plain loads would not do: a whole recovery can pass between them, and
 * the flags of the running thread paired with the timestamp of the recovered
 * one move the bounds of [DtA] 4.5 past the cells that thread still holds.
 */
static void dta_read_rec(const struct lsv_dta_state *dta, unsigned int tid, u64 *ts, u64 *af)
{
	const struct lsv_dta_global_rec *rec = &dta->global[tid];

	do {
		*ts = READ_ONCE(rec->timestamp);
		smp_rmb();
		*af = READ_ONCE(rec->anchor_and_flags);
		ht_record_flags_read(tid);
		smp_rmb();
	} while (READ_ONCE(rec->timestamp) != *ts);
}

/*
 * [DtA] 4.5. A cell is freed only when
 *   MAX(ts of recovered) < retire_ts < MIN(ts of running or stuck)
 *
 * |out_suspected| reports whether any thread carries the STUCK bit, that is,
 * is stuck or recovered but has not resumed its operation yet. Only such a
 * thread can still hold a pointer the timestamps do not account for, so the
 * cells put aside for it become ordinary candidates once none is left.
 */
static void dta_reclaim_bounds(const struct lsv_dta_state *dta, u64 *out_min_ts,
			       u32 *out_min_tid, u64 *out_max_rec_ts,
			       bool *out_suspected)
{
	u64 min_ts = U64_MAX;
	bool suspected = false;
	u64 max_rec = 0;
	u32 min_tid = 0;
	unsigned int i;
	u64 af;
	u64 ts;

	for (i = 0; i < dta->nr_threads; i++) {
		dta_read_rec(dta, i, &ts, &af);

		if (lsv_dta_af_is_stuck(af))
			suspected = true;

		if (lsv_dta_af_is_recovered(af)) {
			if (ts > max_rec)
				max_rec = ts;
		} else if (!lsv_dta_af_is_idle(af) && ts < min_ts) {
			/* strict < keeps the smallest id among equal timestamps */
			min_ts = ts;
			min_tid = i;
		}
	}

	*out_min_ts = min_ts;
	*out_min_tid = min_tid;
	*out_max_rec_ts = max_rec;
	*out_suspected = suspected;
}

/*
 * Cells a stuck or recovered thread may still reach wait here instead of being
 * rescanned on every pass ([DtA] 4.5 keeps them for good; the same section
 * notes they can be reclaimed once their thread has resumed, which is what
 * dta_try_reclaim() does with this list).
 */
static void dta_keep_frozen(struct lsv_dta_local_rec *loc, struct lsv_lf_list_node *node)
{
	node->retired_frozen = true;
	node->removed_link = NULL;

	if (loc->frozen_tail)
		loc->frozen_tail->removed_link = node;
	else
		loc->frozen_head = node;

	loc->frozen_tail = node;
	loc->frozen_cnt++;
}

static void dta_retire_push(struct lsv_dta_local_rec *loc, struct lsv_lf_list_node *node)
{
	node->removed_link = NULL;

	if (loc->retire_tail)
		loc->retire_tail->removed_link = node;
	else
		loc->retire_head = node;

	loc->retire_tail = node;
	loc->retire_cnt++;
}

/* O(1): the whole waiting list joins the queue once nobody is suspected. */
static void dta_retire_splice_frozen(struct lsv_dta_local_rec *loc)
{
	if (!loc->frozen_head)
		return;

	if (loc->retire_tail)
		loc->retire_tail->removed_link = loc->frozen_head;
	else
		loc->retire_head = loc->frozen_head;

	loc->retire_tail = loc->frozen_tail;
	loc->retire_cnt += loc->frozen_cnt;

	loc->frozen_head = NULL;
	loc->frozen_tail = NULL;
	loc->frozen_cnt = 0;
}

/*
 * A cell belongs to the list it was made for, whether it was ever published in
 * it or not, and that list's ops take it back. Cells are never recycled inside
 * the table: one is made by node_alloc() or node_copy() and released by
 * node_dealloc(), once each, so that an implementation counting the cells that
 * point at a value can rely on being told about every one of them.
 */
static void dta_cell_free(struct lsv_dta_state *dta, struct lsv_lf_list_node *node)
{
	dta->ops->node_dealloc(node, dta->ctx);
}

/*
 * The bucket list a participant's anchor lives in. A cell and every copy of it
 * hash there by their key; a head sentinel carries the reserved key of all
 * heads, and is its bucket. No anchor is ever the tail sentinel: a traversal
 * and a recovery stop before the tail, and neither anchors on it.
 */
static struct lsv_lf_list *dta_anchor_list(const struct lsv_dta_state *dta,
					   struct lsv_lf_list_node *anchor)
{
	if (anchor->key == LSV_HT_KEY_HEAD_SENTINEL)
		return container_of(anchor, struct lsv_lf_list, head);

	return ht_bucket(dta->ht, anchor->key);
}

/*
 * [DtA] 4.3, 4.5. Buffers the cell of a list it has been unlinked from. A cell
 * retired concurrently with a recovery transition, or already frozen, waits in
 * a separate list until no thread is stuck or recovered any more.
 */
static void dta_retire_node(struct lsv_dta_state *dta, int tid,
			    struct lsv_lf_list_node *node, bool frozen)
{
	struct lsv_dta_local_rec *loc = &dta->local[tid];
	s64 seq = atomic64_read(&dta->recovery_seq);

	if (atomic_read(&dta->recovery_active))
		frozen = true;

	node->retire_ts = dta_calc_max_ts(dta) + 1;

	if (atomic64_read(&dta->recovery_seq) != seq)
		frozen = true;

	if (frozen) {
		dta_keep_frozen(loc, node);
		return;
	}

	node->retired_frozen = false;
	dta_retire_push(loc, node);
}

/*
 * Retires every cell from the one first points at, tags stripped, up to but
 * not including last, once the caller's CAS has unlinked them all. A cell
 * whose next pointer is frozen is retired as frozen.
 */
void dta_retire_span(struct lsv_dta_state *dta, int tid,
			    struct lsv_lf_list_node *first, struct lsv_lf_list_node *last)
{
	struct lsv_lf_list_node *node;
	struct lsv_lf_list_node *raw;

	for (node = lsv_ht_strip_all(first); node != last; node = lsv_ht_strip_all(raw)) {
		raw = READ_ONCE(node->next);
		dta_retire_node(dta, tid, node, lsv_ht_is_frozen(raw));
	}
}

/* Copies that were never published go straight back to the ops that made them. */
static void dta_drop_copies(struct lsv_dta_state *dta, struct lsv_lf_list_node *copy)
{
	struct lsv_lf_list_node *nxt;

	while (copy) {
		nxt = copy->next;
		dta_cell_free(dta, copy);
		copy = nxt;
	}
}

/*
 * A copy carries the key and the very same value as its source, which is what
 * node_copy() is for: the value is the identity of the entry, and a cell owns
 * no reference to it. The table sets everything about a cell except its key
 * and its value, so that the only two calls that ever give a cell a value are
 * node_alloc() and node_copy().
 *
 * node_copy() is called only to recover a stalled participant, and a recovery
 * that cannot copy cannot finish; it is up to the implementation to keep it
 * served when the table is full (see LSV_DTA_SPARE_CELLS).
 */
static struct lsv_lf_list_node *dta_copy_node(struct lsv_dta_state *dta, int my_tid,
					      struct lsv_lf_list_node *src, u64 insert_ts)
{
	struct lsv_lf_list_node *copy = dta->ops->node_copy(src, dta->ctx);

	if (!copy)
		return NULL;

	copy->insert_ts = insert_ts;
	copy->copy_src = src;
	copy->retired_frozen = false;
	copy->retire_ts = 0;
	copy->next = NULL;

	return copy;
}

/*
 * What one pass of a recovery did to the range it froze ([DtA] Listing 6).
 *
 * A pass copies as many live cells as the cells in hand allow, so a range
 * wider than that is replaced a prefix at a time: |chunk_end| is where this
 * pass stopped, and equals |first_non_frozen| once the whole range has been
 * accounted for.
 */
struct lsv_dta_range_pass {
	/* the first cell beyond the frozen range */
	struct lsv_lf_list_node *first_non_frozen;
	/* the frozen value of the anchor's own next pointer */
	struct lsv_lf_list_node *anchor_next;
	/* the first cell of the range the copies do not stand for */
	struct lsv_lf_list_node *chunk_end;
	/* copies of the live cells before |chunk_end|, in list order */
	struct lsv_lf_list_node *copies;
};

/*
 * The first pass of [DtA] Listing 6: freezes the range, from the anchor's own
 * next pointer to LSV_DTA_ANCHOR_THRESHOLD cells older than the stalled
 * operation, or to the end of the list.
 *
 * The range starts at the anchor itself, its own next pointer included: that
 * pointer is the first thing the stalled thread reads when it wakes, and a
 * cell it reaches through an unfrozen pointer is one the recovery has just
 * made reclaimable under it. Freezing it also closes the range to insertions,
 * since an insertion updates the next pointer of the cell it follows.
 *
 * A frozen pointer takes no update, so once a cell is frozen nothing can be
 * inserted behind it and it can no longer be marked deleted: when this pass
 * returns, the contents of the range are settled: what a pass finds live is
 * what it has to copy.
 *
 * Returns false when the recoverer itself was suspected on the way; whatever
 * is frozen by then stays frozen, for the next helper to carry on with.
 */
static bool dta_freeze_range(struct lsv_lf_list *list, struct lsv_dta_state *dta,
			     int my_tid, int stuck_tid, struct lsv_lf_list_node *cur,
			     u64 low_ts, struct lsv_dta_range_pass *pass)
{
	struct lsv_lf_list_node *anchor = cur;
	int cnt = 0;
	struct lsv_lf_list_node *saved_next;

	pass->anchor_next = NULL;

	while (cnt < LSV_DTA_ANCHOR_THRESHOLD && cur && cur != dta->tail) {
		if (cur != &list->head)
			dta_recovery_step(dta, cur);
		saved_next = READ_ONCE(cur->next);
		while (!lsv_ht_is_frozen(saved_next)) {
			(void)lsv_ht_lcas(&cur->next, saved_next,
					  lsv_ht_freeze_node(saved_next));
			saved_next = READ_ONCE(cur->next);
		}

		/* a frozen pointer is never NULL: this is the first cell */
		if (!pass->anchor_next)
			pass->anchor_next = saved_next;

		/*
		 * The head sentinel belongs to the list and carries no entry,
		 * so it is frozen like the rest of the range but never copied;
		 * the splice overwrites its frozen next pointer.
		 */
		if (cur != &list->head) {
			/*
			 * Count what the stalled lookup counted: its count starts
			 * over after the anchor and skips deleted cells, and it
			 * reads the next pointer of the cell it would anchor at
			 * before it does. A mark is never cleared, so a cell
			 * deleted since the lookup passed it only widens the range.
			 */
			if (cur != anchor && !lsv_ht_has_mark(saved_next) &&
			    READ_ONCE(cur->insert_ts) < low_ts)
				cnt++;

			/* keep the recoverer's own range recoverable */
			if (my_tid != stuck_tid && !dta_set_anchor(dta, my_tid, cur))
				return false;
		}

		cur = lsv_ht_strip_all(saved_next);
	}

	pass->first_non_frozen = cur;

	return true;
}

/*
 * The second pass: copies of the live cells of the frozen range, in list
 * order, at most |budget| of them. The range no longer changes, so the cells
 * for this were secured beforehand and the copies are made without asking the
 * allocator for anything the pass does not already hold.
 *
 * |pass->chunk_end| ends up being the first cell of the range the copies do
 * not stand for. A budget that covers the range leaves it at the end of the
 * range, and everything else leaves the copies standing for a prefix of it,
 * with the rest still frozen for a later pass to replace.
 */
static void dta_copy_range(struct lsv_lf_list *list, struct lsv_dta_state *dta, int my_tid,
			   struct lsv_lf_list_node *cur, u32 budget,
			   struct lsv_dta_range_pass *pass)
{
	struct lsv_lf_list_node *local_head = NULL;
	struct lsv_lf_list_node *local_tail = NULL;
	u64 max_ts = dta_calc_max_ts(dta);
	u32 copied = 0;
	struct lsv_lf_list_node *copy;
	struct lsv_lf_list_node *raw;

	while (cur != pass->first_non_frozen) {
		if (cur != &list->head)
			dta_recovery_step(dta, cur);
		raw = READ_ONCE(cur->next);

		if (cur != &list->head && !lsv_ht_has_mark(raw)) {
			if (copied == budget)
				break;

			copy = dta_copy_node(dta, my_tid, cur, max_ts + 1);
			if (!copy)
				break;

			if (!local_head)
				local_head = copy;
			else
				local_tail->next = copy;
			local_tail = copy;
			copied++;
		}

		cur = lsv_ht_strip_all(raw);
	}

	pass->chunk_end = cur;
	pass->copies = local_head;
}

/*
 * [DtA] Listing 6. Freezes the cells a stalled thread may still reach from its
 * anchor and copies the live ones.
 *
 * Freezing cannot be undone, and a range left frozen is a range no operation
 * can remove from, so what is frozen here has to be replaced sooner or later
 * -- but not necessarily by this pass. Copies are made for as much of the
 * range as the cells in hand cover, and the rest waits, frozen, for the next
 * pass: a pass that ends short leaves the participant suspected, and that is
 * what brings the next helper in.
 */
static bool dta_freeze_nodes(struct lsv_lf_list *list, struct lsv_dta_state *dta,
			     int my_tid, int stuck_tid, u64 old_af,
			     struct lsv_dta_range_pass *pass)
{
	u64 low_ts = READ_ONCE(dta->global[stuck_tid].low_timestamp);
	struct lsv_lf_list_node *anchor = lsv_dta_af_ptr(old_af);

	if (!dta_freeze_range(list, dta, my_tid, stuck_tid, anchor, low_ts, pass))
		return false;

	dta_copy_range(list, dta, my_tid, anchor, LSV_DTA_SPARE_LIMIT, pass);

	return true;
}

/*
 * The splice step of [DtA] Listing 7. Looks for the first copy in |copies|
 * whose source is still reachable, and for the last live unfrozen cell
 * preceding that source; returns the copy and the next-pointer value the
 * source was reached through.
 *
 * Ranges of two stalled threads may overlap, and the other recovery may have
 * already replaced a prefix of ours: the sources it bypassed are unreachable,
 * but the rest of our range is still frozen and must be replaced here.
 *
 * LSV_DTA_FIND_DONE: no source is reachable, other threads replaced them all.
 * LSV_DTA_FIND_ABORT: another frozen range precedes the source, or our anchor was
 * lost. Neither is helped from here: this runs inside a recovery, and a help
 * started from it may pick the very participant being recovered -- the only
 * one suspected, as a rule -- and recurse into the same recovery with no
 * bound on the depth. The pass gives up instead and leaves the participant
 * suspected; the next operation to meet a frozen cell helps from its top level.
 */
static enum lsv_dta_find_rc dta_find_in_recovery(struct lsv_lf_list *list,
						 struct lsv_dta_state *dta, int my_tid,
						 int stuck_tid, struct lsv_lf_list_node *copies,
						 struct lsv_lf_list_node **match,
						 struct lsv_lf_list_node **splice_prev,
						 struct lsv_lf_list_node **old_next)
{
	struct lsv_lf_list_node *prev_next = READ_ONCE(list->head.next);
	struct lsv_lf_list_node *cur = lsv_ht_strip_all(prev_next);
	struct lsv_lf_list_node *prev = &list->head;
	struct lsv_lf_list_node *want = copies;
	bool blocked = false;
	struct lsv_lf_list_node *raw;

	while (cur && cur != dta->tail) {
		dta_recovery_step(dta, cur);
		raw = READ_ONCE(cur->next);

		/* both the list and the copies are sorted by key */
		while (want && want->key < cur->key)
			want = want->next;

		if (want && cur == want->copy_src) {
			if (blocked)
				return LSV_DTA_FIND_ABORT;
			*match = want;
			*splice_prev = prev;
			*old_next = prev_next;
			return LSV_DTA_FIND_FOUND;
		}

		if (!lsv_ht_has_mark(raw)) {
			if (lsv_ht_is_frozen(raw)) {
				/* a live frozen cell of some other range */
				blocked = true;
			} else if (!blocked) {
				/*
				 * A self-recovering thread is stuck and cannot drop
				 * anchors; its timestamp protects this traversal.
				 */
				if (my_tid != stuck_tid && !dta_set_anchor(dta, my_tid, prev))
					return LSV_DTA_FIND_ABORT;
				prev = cur;
				prev_next = raw;
			}
		}

		cur = lsv_ht_strip_all(raw);
	}

	return LSV_DTA_FIND_DONE;
}

/*
 * [DtA] Listing 7. Splices the copies in place of the part of the frozen range
 * they stand for, which ends at |chunk_end|. Cells the splice bypassed are
 * retired by the splicing thread: deleted unfrozen ones normally, frozen
 * lsvginals for good (4.5, "frozen nodes are not reclaimed").
 *
 * Returns false when the splice could not be done within the retry budget, or
 * had to be given up (LSV_DTA_FIND_ABORT); the stalled thread then stays suspected
 * for another helper to finish.
 */
static bool dta_replace_frozen_nodes(struct lsv_lf_list *list, struct lsv_dta_state *dta,
				     int my_tid, int stuck_tid,
				     struct lsv_lf_list_node *new_list,
				     struct lsv_lf_list_node *chunk_end)
{
	u32 retry_cnt = 0;
	struct lsv_lf_list_node *splice_prev;
	struct lsv_lf_list_node *copy_tail;
	struct lsv_lf_list_node *old_next;
	struct lsv_lf_list_node *match;
	struct lsv_lf_list_node *node;
	enum lsv_dta_find_rc rc;

	copy_tail = new_list;
	while (copy_tail->next)
		copy_tail = copy_tail->next;

	for (;;) {
		if (++retry_cnt > LSV_HT_MAX_LOOKUP_RETRIES) {
			dta_drop_copies(dta, new_list);
			return false;
		}

		rc = dta_find_in_recovery(list, dta, my_tid, stuck_tid, new_list, &match,
					  &splice_prev, &old_next);
		if (rc == LSV_DTA_FIND_ABORT) {
#if IS_ENABLED(CONFIG_KUNIT)
			atomic64_inc(&dta->recovery_aborts);
#endif
			dta_drop_copies(dta, new_list);
			return false;
		}
		if (rc == LSV_DTA_FIND_DONE) {
			dta_drop_copies(dta, new_list);
			return true;
		}

		/* published together with the splice */
		copy_tail->next = chunk_end;

		if (lsv_ht_lcas(&splice_prev->next, old_next, match) == old_next)
			break;

		copy_tail->next = NULL;
	}

	/* copies of sources another recovery already replaced were never published */
	while (new_list != match) {
		node = new_list->next;
		dta_cell_free(dta, new_list);
		new_list = node;
	}

	dta_retire_span(dta, my_tid, old_next, chunk_end);

	return true;
}

/*
 * Moves a suspected participant's anchor to where a pass stopped. The cells
 * before it have copies of their own and were retired frozen, so they are held
 * out of reclamation for as long as anyone is suspected, which is the whole
 * point of freezing them; leaving the anchor where it was would only make the
 * next pass copy them again and find none of them reachable.
 *
 * The record is CAS-ed by its exact value, so a pass that ran against an
 * anchor another pass has already moved changes nothing.
 */
static void dta_advance_anchor(struct lsv_dta_state *dta, int stuck_tid, u64 old_ts,
			       u64 stuck_af, struct lsv_lf_list_node *chunk_end)
{
	struct lsv_dta_global_rec *rec = &dta->global[stuck_tid];

	(void)dta_dwcas(rec, old_ts, stuck_af, old_ts,
			lsv_dta_af_make(chunk_end, false, true));
}

/*
 * A range whose cells are all dead leaves no copies to splice it out with, and
 * the frozen pointer it starts from would then stay frozen for good. Only the
 * head sentinel can be the live predecessor of such a range -- an unmarked
 * cell inside it would have been copied -- and a lookup refuses to unlink
 * through a frozen predecessor, so a bucket whose head keeps a frozen next
 * pointer turns away every later operation, for as long as the table lives.
 *
 * Anywhere else the range ends up behind a deleted cell, which leaves the
 * unfrozen pointer of some earlier cell in front of it: an ordinary lookup
 * unlinks it there, and retires it frozen like this does.
 *
 * The anchor's frozen next pointer is CAS-ed by its exact value, so a range
 * another recovery has replaced in the meantime is left alone.
 */
static void dta_drop_dead_range(struct lsv_lf_list *list, struct lsv_dta_state *dta,
				int my_tid, struct lsv_lf_list_node *anchor,
				struct lsv_lf_list_node *anchor_next,
				struct lsv_lf_list_node *first_non_frozen)
{
	if (anchor != &list->head || !anchor_next)
		return;

	if (lsv_ht_lcas(&list->head.next, anchor_next, first_non_frozen) != anchor_next)
		return;

	dta_retire_span(dta, my_tid, anchor_next, first_non_frozen);
}

/*
 * [DtA] Listing 5.
 *   Phase 1: mark stuck_tid suspected.
 *   Phase 2: freeze and copy the range reachable from its anchor.
 *   Phase 3: splice the copies in.
 *   Phase 4: mark it recovered with a fresh timestamp.
 */
static void dta_recover(struct lsv_dta_state *dta, int stuck_tid, int my_tid,
			u64 old_ts, u64 old_af)
{
	struct lsv_dta_global_rec *rec = &dta->global[stuck_tid];
	struct lsv_lf_list_node *anchor = lsv_dta_af_ptr(old_af);
	struct lsv_dta_range_pass pass;
	u64 stuck_af;
	bool marked;
	u64 cur_ts;
	u64 cur_af;
	u64 new_ts;

	/*
	 * An idle record owns no traversal. dta_try_reclaim() names the thread
	 * from a pair read a moment ago, and it may have finished its operation
	 * since; marking that one suspected would clear its idle bit
	 * and freeze a range nobody is walking, from an anchor that is no
	 * longer protected against reclamation.
	 */
	if (lsv_dta_af_is_idle(old_af))
		return;

	stuck_af = lsv_dta_af_make(anchor, false, true);

	dta_recovery_enter(dta);
	marked = dta_dwcas(rec, old_ts, old_af, old_ts, stuck_af);
	dta_recovery_exit(dta);

	if (!marked) {
		cur_ts = READ_ONCE(rec->timestamp);
		cur_af = READ_ONCE(rec->anchor_and_flags);
		/* the thread made progress, or its recovery is already complete */
		if (cur_ts != old_ts || cur_af != stuck_af)
			return;
	}

	if (anchor) {
		struct lsv_lf_list *list = dta_anchor_list(dta, anchor);

		/*
		 * We became suspected ourselves. Freezing cannot be undone, so
		 * the thread stays suspected: the next helper repeats the
		 * recovery over the already frozen cells.
		 */
		if (!dta_freeze_nodes(list, dta, my_tid, stuck_tid, old_af, &pass))
			return;

		if (pass.copies) {
			if (!dta_replace_frozen_nodes(list, dta, my_tid, stuck_tid,
						      pass.copies, pass.chunk_end))
				return;
		} else if (pass.chunk_end == pass.first_non_frozen) {
			dta_drop_dead_range(list, dta, my_tid, anchor, pass.anchor_next,
					    pass.first_non_frozen);
		}

		/*
		 * A pass that could not carry the whole range replaced a prefix
		 * of it and leaves the participant suspected: the rest is still
		 * frozen, and the next pass -- this thread's or a helper's --
		 * starts where this one stopped. The recovery is over only once
		 * nothing of the range is left frozen.
		 */
		if (pass.chunk_end != pass.first_non_frozen) {
			if (pass.copies)
				dta_advance_anchor(dta, stuck_tid, old_ts, stuck_af,
						   pass.chunk_end);
			return;
		}
	}

	/* a failed CAS means another thread has completed this phase */
	dta_recovery_enter(dta);
	new_ts = dta_calc_max_ts(dta) + 1;
	if (dta_dwcas(rec, old_ts, stuck_af, new_ts,
		      lsv_dta_af_make(anchor, true, true)))
		atomic64_inc(&dta->recovery_cnt);
	dta_recovery_exit(dta);
}

/*
 * [DtA] Listing 4. |except| is a participant not to help, the one whose
 * recovery the caller has just seen give up; -1 for none.
 */
static void dta_help_recovery_except(struct lsv_dta_state *dta, int my_tid, int except)
{
	struct lsv_dta_local_rec *loc = &dta->local[my_tid];
	unsigned int i;
	unsigned int n;
	u64 my_ts;
	u64 my_af;
	u64 ts;
	u64 af;

	my_ts = READ_ONCE(dta->global[my_tid].timestamp);
	my_af = READ_ONCE(dta->global[my_tid].anchor_and_flags);

	/* a recovered thread only restarts its operation (Figure 1) */
	if (lsv_dta_af_is_stuck(my_af)) {
		if (!lsv_dta_af_is_recovered(my_af))
			dta_recover(dta, my_tid, my_tid, my_ts, my_af);
		dta_begin_op(dta, my_tid, NULL);
		return;
	}

	/*
	 * Start after the participant this thread helped last. A recovery that
	 * gives up on a range frozen for another participant must not keep
	 * every helper on itself, or the other one is never helped.
	 */
	for (n = 0; n < dta->nr_threads; n++) {
		i = (loc->help_next + n) % dta->nr_threads;
		if ((int)i == except)
			continue;
		af = READ_ONCE(dta->global[i].anchor_and_flags);
		if (lsv_dta_af_is_suspected(af)) {
			ts = READ_ONCE(dta->global[i].timestamp);
			loc->help_next = i + 1;
			dta_recover(dta, i, my_tid, ts, af);
			return;
		}
	}
}

void dta_help_recovery(struct lsv_dta_state *dta, int my_tid)
{
	dta_help_recovery_except(dta, my_tid, -1);
}

/*
 * A thread this one keeps waiting for is suspected only after both hold. One
 * that is suspected already has had its verdict: a recovery left unfinished by
 * a pass that could not carry the whole range is due again at once.
 */
static bool dta_recovery_due(struct lsv_dta_local_rec *loc, u64 af)
{
	unsigned long waited = jiffies - loc->min_timestamp_at;

	if (lsv_dta_af_is_suspected(af))
		return true;

	return loc->min_timestamp_cnt >= LSV_DTA_RECOVERY_THRESHOLD &&
	       waited >= msecs_to_jiffies(LSV_DTA_RECOVERY_DELAY_MS);
}

/*
 * [DtA] 4.3. Frees the buffered cells whose retire timestamp satisfies 4.5;
 * suspects the thread that keeps blocking the rest.
 *
 * One call examines at most LSV_DTA_RECLAIM_BUDGET cells, so the maintenance
 * an operation pays for is bounded no matter how long the queue is. Cells that
 * stay are appended behind it, and the next call carries on with the ones it
 * has not seen.
 *
 * Cells set aside for a stuck or recovered thread rejoin the queue as soon as
 * there is none: a thread that has resumed its operation restarted it from the
 * head of a bucket and holds nothing it held before ([DtA] 4.5, "frozen nodes
 * can be easily reclaimed for recovered threads that have resumed operation").
 *
 * Returns the number of cells given back.
 */
static u32 dta_try_reclaim(struct lsv_dta_state *dta, int tid)
{
	struct lsv_dta_local_rec *loc = &dta->local[tid];
	bool blocked = false;
	u32 freed = 0;
	struct lsv_lf_list_node *cur;
	u64 max_rec_ts;
	bool suspected;
	u32 min_tid;
	u64 min_ts;
	u32 todo;
	u64 af;
	u64 ts;

	dta_reclaim_bounds(dta, &min_ts, &min_tid, &max_rec_ts, &suspected);

	if (!suspected)
		dta_retire_splice_frozen(loc);

	todo = min_t(u32, loc->retire_cnt, LSV_DTA_RECLAIM_BUDGET);

	while (todo--) {
		cur = loc->retire_head;
		loc->retire_head = cur->removed_link;
		if (!loc->retire_head)
			loc->retire_tail = NULL;
		loc->retire_cnt--;

		if (suspected && (cur->retired_frozen ||
				  lsv_ht_is_frozen(READ_ONCE(cur->next)))) {
			dta_keep_frozen(loc, cur);
			continue;
		}

		if (max_rec_ts < cur->retire_ts && cur->retire_ts < min_ts) {
			dta_cell_free(dta, cur);
			freed++;
			continue;
		}

		blocked = true;
		dta_retire_push(loc, cur);
	}

	if (!blocked || min_ts == U64_MAX) {
		loc->min_timestamp = 0;
		loc->min_timestamp_tid = 0;
		loc->min_timestamp_cnt = 0;
	} else if (min_ts == loc->min_timestamp && min_tid == loc->min_timestamp_tid) {
		loc->min_timestamp_cnt++;
		dta_read_rec(dta, min_tid, &ts, &af);
		if (dta_recovery_due(loc, af)) {
			if (min_tid != tid && ts == min_ts) {
				dta_recover(dta, min_tid, tid, ts, af);
				/*
				 * Still suspected: its pass gave up, maybe on a range
				 * frozen for another suspected participant. Only an
				 * operation that meets that range helps its owner, and
				 * the table may see none; the thread that keeps the
				 * bounds held does, or nothing ever moves again.
				 */
				af = READ_ONCE(dta->global[min_tid].anchor_and_flags);
				if (lsv_dta_af_is_suspected(af))
					dta_help_recovery_except(dta, tid, min_tid);
			}
			loc->min_timestamp_cnt = 0;
			loc->min_timestamp_at = jiffies;
		}
	} else {
		loc->min_timestamp = min_ts;
		loc->min_timestamp_tid = min_tid;
		loc->min_timestamp_cnt = 1;
		loc->min_timestamp_at = jiffies;
	}

	return freed;
}

static struct lsv_dta_state *dta_of_local(struct lsv_dta_local_rec *loc, unsigned int tid)
{
	return container_of(loc - tid, struct lsv_dta_local_block, rec[0])->dta;
}

/*
 * A reclaim request, on the worker of the participant that owns the buffer:
 * a scan in an operation of its own, between two of the owner's, so that the
 * buffer never has two threads at once.
 */
static void dta_reclaim_request_run(struct lsv_wq_ctx *ctx)
{
	struct lsv_dta_local_rec *loc = container_of(ctx, struct lsv_dta_local_rec,
							   reclaim_ctx);
	unsigned int tid = lsv_worker_id(lsv_wq_current_worker());
	struct lsv_dta_state *dta = dta_of_local(loc, tid);

	/* the table was freed on this very worker, and left this to free */
	if (!dta) {
		kvfree(container_of(loc - tid, struct lsv_dta_local_block, rec[0]));
		return;
	}

	/* a request that comes in from now on is queued anew */
	atomic_set(&loc->reclaim_queued, 0);

	dta_begin_op(dta, tid, NULL);
	dta_try_reclaim(dta, tid);
	dta_end_op(dta, tid);

	/* the last touch of the table: lsv_ht_free() may go on after it */
	atomic_dec(&dta->reclaim_pending);
}

/*
 * Asks every other participant that holds buffered cells to scan its buffer.
 * The buffer is its owner's alone, and an owner that no longer operates on
 * the table would keep what it buffered for as long as the table lives: after
 * a stall, that may be everything it retired meanwhile.
 */
void dta_request_reclaim(struct lsv_dta_state *dta, int my_tid)
{
	struct lsv_dta_local_rec *loc;
	struct lsv_worker *worker;
	unsigned int i;

	for (i = 0; i < dta->nr_threads; i++) {
		loc = &dta->local[i];
		if ((int)i == my_tid ||
		    (!READ_ONCE(loc->retire_cnt) && !READ_ONCE(loc->frozen_cnt)))
			continue;
		if (atomic_xchg(&loc->reclaim_queued, 1))
			continue;

		worker = lsv_worker_by_id(i);
		if (!worker) {
			atomic_set(&loc->reclaim_queued, 0);
			continue;
		}
		atomic_inc(&dta->reclaim_pending);
		lsv_worker_add(worker, &loc->reclaim_ctx);
	}
}

struct lsv_dta_state *dta_alloc(struct lsv_ht *ht)
{
	struct lsv_dta_local_block *block;
	struct lsv_dta_state *dta;
	unsigned int i;

	dta = kzalloc_obj(*dta);
	if (!dta)
		return NULL;

	dta->nr_threads = max_t(unsigned int, lsv_worker_id_limit(), 1);

	dta->global = kvzalloc_objs(*dta->global, dta->nr_threads);
	block = kvzalloc(struct_size(block, rec, dta->nr_threads), GFP_KERNEL);
	/*
	 * A double-width compare-and-swap faults on an unaligned address, and
	 * the allocator promises the alignment of the type only for sizes it
	 * rounds to a power of two.
	 */
	if (!dta->global || !block ||
	    WARN_ON((uintptr_t)dta->global % sizeof(u128))) {
		kvfree(dta->global);
		kvfree(block);
		kfree(dta);
		return NULL;
	}
	block->dta = dta;
	dta->local = block->rec;

	for (i = 0; i < dta->nr_threads; i++) {
		dta->global[i].anchor_and_flags = LSV_DTA_IDLE_BIT;
		lsv_wq_ctx_init(&dta->local[i].reclaim_ctx, dta_reclaim_request_run);
	}

	dta->ht = ht;
	dta->tail = &ht->tail;
	dta->ops = ht->ops;
	dta->ctx = ht->ctx;

	return dta;
}

/*
 * |own_request|: a reclaim request is still queued on the worker this runs
 * on. It runs only after this returns, and frees the local records then.
 */
void dta_free(struct lsv_dta_state *dta, bool own_request)
{
	struct lsv_dta_local_block *block = container_of(dta->local,
							       struct lsv_dta_local_block,
							       rec[0]);
	struct lsv_lf_list_node *node;
	struct lsv_lf_list_node *nxt;
	unsigned int i;

	for (i = 0; i < dta->nr_threads; i++) {
		for (node = dta->local[i].retire_head; node; node = nxt) {
			nxt = node->removed_link;
			dta_cell_free(dta, node);
		}
		for (node = dta->local[i].frozen_head; node; node = nxt) {
			nxt = node->removed_link;
			dta_cell_free(dta, node);
		}
	}

	kvfree(dta->global);
	if (own_request)
		block->dta = NULL;
	else
		kvfree(block);
	kfree(dta);
}

/*
 * [DtA] 4.1: participants are the lsv_wq workers created before the table.
 * Any other caller fails its operation.
 */
int ht_tid(const struct lsv_ht *ht)
{
	struct lsv_worker *worker = lsv_wq_current_worker();
	unsigned int id;

	if (WARN_ON_ONCE(!worker))
		return -1;

	id = lsv_worker_id(worker);
	if (unlikely(id >= ht->dta->nr_threads)) {
		/*
		 * The table sized its records when it was created, so a worker
		 * of a queue created after it has no place among the
		 * participants and every operation of it fails. Say so more
		 * than once: the table serves that worker nothing for as long
		 * as it lives.
		 */
		pr_warn_ratelimited("lsv_ht: worker %u is not a participant of a table sized for %u\n",
				    id, ht->dta->nr_threads);
		return -1;
	}

	return id;
}

/*
 * |no_cell| is an operation that node_alloc() turned away: the buffered cells
 * are scanned then whatever the thresholds say, since some of them may be the
 * very cells the allocator is out of. Returns the number of cells given back.
 */
u32 ht_op_end(struct lsv_ht *ht, int tid, bool no_cell)
{
	struct lsv_dta_state *dta = ht->dta;
	struct lsv_dta_local_rec *loc = &dta->local[tid];
	u32 freed = 0;

	/*
	 * Reclamation may run a recovery that moves our anchor, so it happens
	 * only here, after every traversal of the operation is over. A table
	 * that fills up has its participants scan their buffers early, but
	 * only those that have anything buffered: a lookup with nothing to give
	 * back would pay for a scan of every record for nothing.
	 */
	if (no_cell || loc->retire_cnt >= LSV_DTA_RETIRE_THRESHOLD ||
	    ((loc->retire_cnt || loc->frozen_cnt) &&
	     lsv_ht_entry_cnt(ht) * 100 >= ht->max_entry_cnt * LSV_HT_DEF_FILL_THRESHOLD))
		freed = dta_try_reclaim(dta, tid);

	dta_end_op(dta, tid);

	return freed;
}

#if IS_ENABLED(CONFIG_KUNIT)
/*
 * The anchor of a participant must not outlive the operation that set it: an
 * idle participant protects nothing, so the cell it names can be freed and
 * reused, and a recovery that catches the record afterwards walks into it.
 */
bool ht_participant_holds_anchor(const struct lsv_ht *ht, unsigned int tid)
{
	u64 af = READ_ONCE(ht->dta->global[tid].anchor_and_flags);

	return !!lsv_dta_af_ptr(af);
}

/*
 * A participant is suspected from the moment a recovery marks it until it has
 * resumed its operation. A pass that ends short of the whole range leaves it
 * this way, for the next pass to carry on with.
 */
bool ht_participant_is_suspected(const struct lsv_ht *ht, unsigned int tid)
{
	u64 af = READ_ONCE(ht->dta->global[tid].anchor_and_flags);

	return lsv_dta_af_is_stuck(af);
}

/* Passes of recoveries that gave up on a range frozen for someone else. */
u64 ht_recovery_aborts(const struct lsv_ht *ht)
{
	return atomic64_read(&ht->dta->recovery_aborts);
}

/* Cells the walks of recoveries have stepped on, freezing, copying and splicing. */
u64 ht_recovery_walked(const struct lsv_ht *ht)
{
	return atomic64_read(&ht->dta->recovery_walked);
}

/* Reclaim requests queued on the workers of participants and not run yet. */
bool ht_reclaim_requests_pending(const struct lsv_ht *ht)
{
	return atomic_read(&ht->dta->reclaim_pending);
}
#endif /* CONFIG_KUNIT */
