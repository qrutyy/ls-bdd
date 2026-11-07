#include <linux/build_bug.h>
#include <linux/preempt.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <kunit/static_stub.h>
#include <kunit/visibility.h>

#include "ebr.h"
#include "tests/include_static/__static_kunit_lsv_ht.h"

/*
 * The largest timestamp taken so far. Idle records count too: they keep the
 * timestamp of their last operation, so the result never goes down.
 */
u64 ebr_calc_max_ts(const struct lsv_ebr_state *ebr)
{
	u64 max_ts = 0;
	unsigned int i;
	u64 ts;

	for (i = 0; i < ebr->nr_threads; i++) {
		ts = lsv_ebr_rec_ts(READ_ONCE(ebr->global[i].state));
		if (ts > max_ts)
			max_ts = ts;
	}

	return max_ts;
}

/*
 * [DtA] 4.1: start an operation with a timestamp above every one taken so far.
 *
 * Preemption stays off until the operation ends. A participant taken off its
 * CPU in the middle of an operation would keep its timestamp in the bound of
 * every scan, and every cell retired meanwhile anywhere in the table out of
 * reach, for as long as it waits for the CPU. Nothing between the two may
 * sleep, see ht_ebr-.h.
 *
 * Only the owner writes its record, so a plain store publishes it. The full
 * barrier behind it pairs with the unlinking CAS of a traversal, which is fully
 * ordered as well: either the traversals of this operation find the cells that
 * CAS unlinked gone, or the retirement that follows it reads this timestamp
 * and stamps the cells above it (ebr_retire_span()).
 *
 * The timestamp may be stale once published: another participant may have
 * begun in between with a larger one. A smaller timestamp only lowers the
 * bound of ebr_try_reclaim(), which then keeps more cells, never fewer.
 */
void ebr_begin_op(struct lsv_ebr_state *ebr, int tid)
{
	u64 ts;

	preempt_disable();
	ts = ebr_calc_max_ts(ebr) + 1;
	smp_store_mb(ebr->global[tid].state, lsv_ebr_rec_make(ts, false));
}

/*
 * [DtA] 4.1: finishing an operation turns the IDLE bit on, and takes the
 * participant out of the bound. The release orders every access the operation
 * made to a cell before it, for the scan that reads the bit and frees the cell.
 */
static void ebr_end_op(struct lsv_ebr_state *ebr, int tid)
{
	struct lsv_ebr_global_rec *rec = &ebr->global[tid];

	smp_store_release(&rec->state, READ_ONCE(rec->state) | LSV_EBR_IDLE_BIT);
	preempt_enable();
}

#if IS_ENABLED(CONFIG_KUNIT)
/*
 * Called on every cell any traversal steps on, from any participant: KUnit
 * checks there that no traversal ever reaches a cell the table has given
 * back. A plain pointer and not a static stub, whose lookup would serialize
 * the traversals of a stress test on the lock of the test.
 */
void (*g_lsv_ht_kunit_cell_check)(struct lsv_lf_list_node *cell);
#endif

/*
 * A traversal has stepped onto a cell and is about to read its next pointer;
 * KUnit checks the cell here, and stalls a participant in the middle of an
 * operation.
 */
void ht_kunit_cell_reached(struct lsv_lf_list_node *cell)
{
#if IS_ENABLED(CONFIG_KUNIT)
	void (*check)(struct lsv_lf_list_node *cell) = READ_ONCE(g_lsv_ht_kunit_cell_check);

	if (check)
		check(cell);
#endif
	KUNIT_STATIC_STUB_REDIRECT(ht_kunit_cell_reached, cell);
}

/*
 * [DtA] 4.5 without its recovered threads. The smallest timestamp of a running
 * participant, U64_MAX when none runs: a cell retired above it may still be
 * reached by that participant, and one retired below it by nobody.
 *
 * The acquire pairs with the release of ebr_end_op(): a participant read idle
 * here is done with every cell it reached.
 */
static u64 ebr_min_running_ts(const struct lsv_ebr_state *ebr)
{
	u64 min_ts = U64_MAX;
	unsigned int i;
	u64 state;

	for (i = 0; i < ebr->nr_threads; i++) {
		state = smp_load_acquire(&ebr->global[i].state);
		if (!lsv_ebr_rec_is_idle(state) && lsv_ebr_rec_ts(state) < min_ts)
			min_ts = lsv_ebr_rec_ts(state);
	}

	return min_ts;
}

static void ebr_retire_push(struct lsv_ebr_local_rec *loc, struct lsv_lf_list_node *node)
{
	node->removed_link = NULL;

	if (loc->retire_tail)
		loc->retire_tail->removed_link = node;
	else
		loc->retire_head = node;

	loc->retire_tail = node;
	loc->retire_cnt++;
}

/*
 * A cell belongs to the list it was made for, whether it was ever published in
 * it or not, and that list's ops take it back. Cells are never recycled inside
 * the table: one is made by node_alloc() and released by node_dealloc(), once
 * each.
 */
static void ebr_cell_free(struct lsv_ebr_state *ebr, struct lsv_lf_list_node *node)
{
	ebr->ops->node_dealloc(node, ebr->ctx);
}

/*
 * Retires every cell from the one |first| points at, tag stripped, up to but
 * not including |last|, once the caller's CAS has unlinked them all.
 *
 * The stamp is read after that CAS, so it is above the timestamp of every
 * participant that could still reach the cells: such a participant published
 * its timestamp before its traversal read the link the CAS replaced, see
 * ebr_begin_op(). One stamp serves the whole span, unlinked by one CAS.
 */
void ebr_retire_span(struct lsv_ebr_state *ebr, int tid,
		     struct lsv_lf_list_node *first, struct lsv_lf_list_node *last)
{
	struct lsv_ebr_local_rec *loc = &ebr->local[tid];
	u64 retire_ts = ebr_calc_max_ts(ebr) + 1;
	struct lsv_lf_list_node *node;
	struct lsv_lf_list_node *raw;

	for (node = lsv_ht_strip_all(first); node != last; node = lsv_ht_strip_all(raw)) {
		raw = READ_ONCE(node->next);
		node->retire_ts = retire_ts;
		ebr_retire_push(loc, node);
	}
}

/*
 * [DtA] 4.3. Frees the buffered cells retired below every running participant.
 *
 * The buffer is sorted by retire timestamp, so the scan stops at the first
 * cell still in range: everything behind it is too. One call frees at most
 * LSV_EBR_RECLAIM_BUDGET cells, so the maintenance an operation pays for is
 * bounded no matter how long the queue is.
 *
 * The scan touches no list, so it runs outside any operation, preemptible:
 * node_dealloc() may sleep.
 *
 * Returns the number of cells given back.
 */
u32 ebr_try_reclaim(struct lsv_ebr_state *ebr, int tid)
{
	struct lsv_ebr_local_rec *loc = &ebr->local[tid];
	u64 min_ts = ebr_min_running_ts(ebr);
	struct lsv_lf_list_node *cur;
	u32 freed = 0;

	while (freed < LSV_EBR_RECLAIM_BUDGET) {
		cur = loc->retire_head;
		if (!cur || cur->retire_ts >= min_ts)
			break;

		loc->retire_head = cur->removed_link;
		if (!loc->retire_head)
			loc->retire_tail = NULL;
		loc->retire_cnt--;

		ebr_cell_free(ebr, cur);
		freed++;
	}

	return freed;
}

static struct lsv_ebr_state *ebr_of_local(struct lsv_ebr_local_rec *loc, unsigned int tid)
{
	return container_of(loc - tid, struct lsv_ebr_local_block, rec[0])->ebr;
}

/*
 * A reclaim request, on the worker of the participant that owns the buffer,
 * between two of the owner's operations, so that the buffer never has two
 * threads at once. A scan touches no list, so it runs outside any operation.
 */
static void ebr_reclaim_request_run(struct lsv_wq_ctx *ctx)
{
	struct lsv_ebr_local_rec *loc = container_of(ctx, struct lsv_ebr_local_rec,
						     reclaim_ctx);
	unsigned int tid = lsv_worker_id(lsv_wq_current_worker());
	struct lsv_ebr_state *ebr = ebr_of_local(loc, tid);

	/* the table was freed on this very worker, and left this to free */
	if (!ebr) {
		kvfree(container_of(loc - tid, struct lsv_ebr_local_block, rec[0]));
		return;
	}

	/* a request that comes in from now on is queued anew */
	atomic_set(&loc->reclaim_queued, 0);

	ebr_try_reclaim(ebr, tid);

	/*
	 * The last touch of the table: lsv_ht_free() may go on after it. The
	 * release keeps the scan before it, for the acquire of lsv_ht_free(),
	 * which frees the very buffer the scan has just written.
	 */
	atomic_dec_return_release(&ebr->reclaim_pending);
}

/*
 * Asks every other participant that holds buffered cells to scan its buffer.
 * The buffer is its owner's alone, and an owner that no longer operates on
 * the table would keep what it buffered for as long as the table lives.
 */
void ebr_request_reclaim(struct lsv_ebr_state *ebr, int my_tid)
{
	struct lsv_ebr_local_rec *loc;
	struct lsv_worker *worker;
	unsigned int i;

	for (i = 0; i < ebr->nr_threads; i++) {
		loc = &ebr->local[i];
		if ((int)i == my_tid || !READ_ONCE(loc->retire_cnt))
			continue;
		if (atomic_xchg(&loc->reclaim_queued, 1))
			continue;

		worker = lsv_worker_by_id(i);
		if (!worker) {
			atomic_set(&loc->reclaim_queued, 0);
			continue;
		}
		atomic_inc(&ebr->reclaim_pending);
		lsv_worker_add(worker, &loc->reclaim_ctx);
	}
}

struct lsv_ebr_state *ebr_alloc(struct lsv_ht *ht)
{
	struct lsv_ebr_local_block *block;
	struct lsv_ebr_state *ebr;
	unsigned int i;

	ebr = kzalloc_obj(*ebr);
	if (!ebr)
		return NULL;

	ebr->nr_threads = max_t(unsigned int, lsv_worker_id_limit(), 1);

	ebr->global = kvzalloc_objs(*ebr->global, ebr->nr_threads);
	block = kvzalloc(struct_size(block, rec, ebr->nr_threads), GFP_KERNEL);
	if (!ebr->global || !block) {
		kvfree(ebr->global);
		kvfree(block);
		kfree(ebr);
		return NULL;
	}
	block->ebr = ebr;
	ebr->local = block->rec;

	for (i = 0; i < ebr->nr_threads; i++) {
		ebr->global[i].state = lsv_ebr_rec_make(0, true);
		lsv_wq_ctx_init(&ebr->local[i].reclaim_ctx, ebr_reclaim_request_run);
	}

	ebr->ht = ht;
	ebr->tail = &ht->tail;
	ebr->ops = ht->ops;
	ebr->ctx = ht->ctx;

	return ebr;
}

/*
 * |own_request|: a reclaim request is still queued on the worker this runs
 * on. It runs only after this returns, and frees the local records then.
 */
void ebr_free(struct lsv_ebr_state *ebr, bool own_request)
{
	struct lsv_ebr_local_block *block = container_of(ebr->local,
							 struct lsv_ebr_local_block,
							 rec[0]);
	struct lsv_lf_list_node *node;
	struct lsv_lf_list_node *nxt;
	unsigned int i;

	for (i = 0; i < ebr->nr_threads; i++) {
		for (node = ebr->local[i].retire_head; node; node = nxt) {
			nxt = node->removed_link;
			ebr_cell_free(ebr, node);
		}
	}

	kvfree(ebr->global);
	if (own_request)
		block->ebr = NULL;
	else
		kvfree(block);
	kfree(ebr);
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
	if (unlikely(id >= ht->ebr->nr_threads)) {
		/*
		 * The table sized its records when it was created, so a worker
		 * of a queue created after it has no place among the
		 * participants and every operation of it fails. Say so more
		 * than once: the table serves that worker nothing for as long
		 * as it lives.
		 */
		pr_warn_ratelimited("lsv_ht: worker %u is not a participant of a table sized for %u\n",
				    id, ht->ebr->nr_threads);
		return -1;
	}

	return id;
}

/* Ends an operation, and scans the buffer of its participant now and then. */
void ht_op_end(struct lsv_ht *ht, int tid)
{
	struct lsv_ebr_state *ebr = ht->ebr;
	struct lsv_ebr_local_rec *loc = &ebr->local[tid];

	ebr_end_op(ebr, tid);

	/*
	 * After the end of the operation, whose own timestamp would otherwise
	 * keep every cell it has just retired. A table that fills up has its
	 * participants scan their buffers early, but only those that have
	 * anything buffered: a lookup with nothing to give back would pay for
	 * a read of every record for nothing.
	 */
	if (loc->retire_cnt >= LSV_EBR_RETIRE_THRESHOLD ||
	    (loc->retire_cnt &&
	     lsv_ht_entry_cnt(ht) * 100 >= ht->max_entry_cnt * LSV_HT_DEF_FILL_THRESHOLD))
		ebr_try_reclaim(ebr, tid);
}

#if IS_ENABLED(CONFIG_KUNIT)
/* Reclaim requests queued on the workers of participants and not run yet. */
bool ht_kunit_reclaim_requests_pending(const struct lsv_ht *ht)
{
	return atomic_read(&ht->ebr->reclaim_pending);
}
#endif /* CONFIG_KUNIT */
