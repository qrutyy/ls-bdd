// SPDX-License-Identifier: GPL-2.0-only

/*
 * ht=ebr: [EBR] with the limbo lists of every participant its own.
 *
 * Each participant keeps three limbo lists, indexed by epoch mod 3, and is the
 * only one ever to touch them: it fills them with plain stores, and frees a
 * list once the global epoch is two past the epoch the list was filled in.
 * Advancing the epoch is a scan and a compare-and-swap, nothing more; freeing
 * is up to each owner, and costs it its own garbage only.
 *
 * The text of [EBR] keeps three lists for the whole system instead, freed by
 * whoever advances the epoch (ebr_global.c, ht=ebr+); most implementations
 * of EBR keep them per thread, as this file does. See docs/ebr.md.
 *
 * What a participant cannot do here is free the garbage of another one, so a
 * participant that no longer operates on the table would keep its cells for
 * as long as the table lives. One that runs out of cells therefore asks the
 * others to reclaim, on their own workers: ebr_request_reclaim().
 */

#include <linux/minmax.h>
#include <linux/overflow.h>
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/slab.h>

#include "ebr.h"
#include "tests/include_static/__static_kunit_lsv_ht.h"

/*
 * A limbo list: the cells a participant retired while it observed |epoch|,
 * linked through lsv_lf_list_node::removed_link.
 */
struct lsv_ebr_limbo {
	struct lsv_lf_list_node *head;
	u64 epoch;
	u32 cnt;
};

/* Per-participant state private to its owner, apart from |reclaim_queued|. */
struct lsv_ebr_local_rec {
	struct lsv_ebr_limbo limbo[LSV_EBR_NR_EPOCHS];
	/* cells in all three limbo lists */
	u32 retire_cnt;
	/* operations ended, which paces the attempts to advance the epoch */
	u32 op_cnt;
	/* |reclaim_ctx| is queued on this participant's worker */
	atomic_t reclaim_queued;
	/*
	 * A reclamation of this participant's limbo lists, asked for by a
	 * participant that ran out of cells: the lists are their owner's alone,
	 * so the reclamation runs on the owner's worker, between two of its
	 * operations.
	 */
	struct lsv_shard_wq_work reclaim_ctx;
} ____cacheline_aligned;

/*
 * The back-end state. A reclaim request finds it from its local record, and
 * may outlive the rest of the table: see ebr_limbo_destroy().
 */
struct lsv_ebr_limbo_state {
	struct lsv_ebr_state *ebr;
	/* reclaim requests queued and not run yet; ebr_limbo_destroy() waits for them */
	atomic_t reclaim_pending;
	struct lsv_ebr_local_rec rec[];
};

static struct lsv_ebr_local_rec *ebr_local(struct lsv_ebr_state *ebr, int tid)
{
	return &ebr->limbo->rec[tid];
}

/*
 * The epoch moves from e to e + 1 once every participant in a critical region
 * has observed e. Returns the epoch as it is now, whoever moved it.
 */
static u64 ebr_try_advance(struct lsv_ebr_state *ebr)
{
	u64 e = atomic64_read(&ebr->epoch);

	if (!ebr_all_observed(ebr, e))
		return e;

	/* a lost race means somebody else moved it: just as good */
	atomic64_cmpxchg(&ebr->epoch, e, e + 1);

	return atomic64_read(&ebr->epoch);
}

static u32 ebr_limbo_free(struct lsv_ebr_state *ebr, struct lsv_ebr_local_rec *loc,
			  struct lsv_ebr_limbo *limbo)
{
	struct lsv_lf_list_node *head = limbo->head;
	u32 freed = limbo->cnt;

	limbo->head = NULL;
	limbo->cnt = 0;
	loc->retire_cnt -= freed;

	ebr_chain_free(ebr, head);

	return freed;
}

/*
 * Frees the limbo lists of |tid| that are two epochs behind |e|, an epoch the
 * caller has read. Run by the owner of the lists, inside or outside of a
 * critical region of its own.
 */
static u32 ebr_reclaim(struct lsv_ebr_state *ebr, int tid, u64 e)
{
	struct lsv_ebr_local_rec *loc = ebr_local(ebr, tid);
	struct lsv_ebr_limbo *limbo;
	u32 freed = 0;
	unsigned int i;

	if (!loc->retire_cnt)
		return 0;

	/* pairs with ebr_exit() via the epoch: last loads of a cell come before its free */
	smp_mb();

	for (i = 0; i < LSV_EBR_NR_EPOCHS; i++) {
		limbo = &loc->limbo[i];
		if (limbo->cnt && limbo->epoch + 2 <= e)
			freed += ebr_limbo_free(ebr, loc, limbo);
	}

	return freed;
}

/*
 * [EBR]: a cell unlinked from its list goes to the limbo list of the epoch
 * its retirer observes after the unlink. The unlinking CAS is a full barrier,
 * so the epoch read here is no older than the unlink.
 *
 * The list of that epoch modulo 3 may still hold the cells of three epochs
 * before: limbo epochs only grow, so a list tagged with another epoch of the
 * same residue is at least three behind, and free to go right away.
 */
static void ebr_retire(struct lsv_ebr_state *ebr, int tid, struct lsv_lf_list_node *node)
{
	struct lsv_ebr_local_rec *loc = ebr_local(ebr, tid);
	u64 e = atomic64_read(&ebr->epoch);
	struct lsv_ebr_limbo *limbo = &loc->limbo[e % LSV_EBR_NR_EPOCHS];

	if (limbo->cnt && limbo->epoch != e) {
		/* pairs with ebr_exit(), as in ebr_reclaim() */
		smp_mb();
		ebr_limbo_free(ebr, loc, limbo);
	}

	limbo->epoch = e;
	node->removed_link = limbo->head;
	limbo->head = node;
	limbo->cnt++;
	loc->retire_cnt++;
}

void ebr_retire_span(struct lsv_ebr_state *ebr, int tid,
		     struct lsv_lf_list_node *first, struct lsv_lf_list_node *last)
{
	struct lsv_lf_list_node *node;
	struct lsv_lf_list_node *next;

	for (node = lsv_ht_strip_mark(first); node != last; node = next) {
		next = lsv_ht_strip_mark(READ_ONCE(node->next));
		ebr_retire(ebr, tid, node);
	}
}

/*
 * Outside of any critical region of |tid|: moves the epoch on as far as the
 * other participants let it and frees what that gave back. Two advances are
 * what a cell retired in the current epoch needs.
 */
static u32 ebr_reclaim_quiescent(struct lsv_ebr_state *ebr, int tid)
{
	u32 freed = 0;
	u64 e;

	e = ebr_try_advance(ebr);
	freed += ebr_reclaim(ebr, tid, e);
	if (!ebr_local(ebr, tid)->retire_cnt)
		return freed;

	e = ebr_try_advance(ebr);
	freed += ebr_reclaim(ebr, tid, e);

	return freed;
}

u32 ebr_op_end(struct lsv_ht *ht, int tid, bool no_cell)
{
	struct lsv_ebr_state *ebr = ht->ebr;
	struct lsv_ebr_local_rec *loc = ebr_local(ebr, tid);
	bool paced;

	ebr_exit(ebr, tid);
	paced = ++loc->op_cnt % LSV_EBR_ADVANCE_FREQ == 0;

	/*
	 * Trying to advance scans every record, so it is paced, unless cells
	 * pile up, the table fills up, or this very operation went without
	 * one. Checking whether a limbo list has come of age is a single load,
	 * and done whenever there is something in limbo.
	 */
	if (no_cell || paced || loc->retire_cnt >= LSV_EBR_RETIRE_THRESHOLD ||
	    (loc->retire_cnt && ebr_table_filling(ht)))
		return ebr_reclaim_quiescent(ebr, tid);

	return ebr_reclaim(ebr, tid, atomic64_read(&ebr->epoch));
}

/*
 * A reclaim request, on the worker of the participant that owns the limbo
 * lists, between two of its operations, so that the lists never have two
 * threads at once.
 */
static void ebr_reclaim_request_run(struct lsv_shard_wq_work *ctx)
{
	struct lsv_ebr_local_rec *loc = container_of(ctx, struct lsv_ebr_local_rec,
						     reclaim_ctx);
	unsigned int tid = lsv_shard_wq_worker_id(lsv_shard_wq_current_worker());
	struct lsv_ebr_limbo_state *limbo = container_of(loc - tid, struct lsv_ebr_limbo_state,
							 rec[0]);
	struct lsv_ebr_state *ebr = limbo->ebr;

	/* the table was freed on this very worker, and left this to free */
	if (!ebr) {
		kvfree(limbo);
		return;
	}

	/* a request that comes in from now on is queued anew */
	atomic_set(&loc->reclaim_queued, 0);

	ebr_reclaim_quiescent(ebr, tid);

	/* the last touch of the table: ebr_limbo_destroy() may go on after it */
	atomic_dec(&limbo->reclaim_pending);
}

/*
 * Asks every other participant with cells in limbo to reclaim them. The lists
 * are their owner's alone, and an owner that no longer operates on the table
 * would keep them for as long as the table lives.
 */
void ebr_request_reclaim(struct lsv_ebr_state *ebr, int my_tid)
{
	struct lsv_ebr_local_rec *loc;
	unsigned int i;

	for (i = 0; i < ebr->nr_threads; i++) {
		loc = ebr_local(ebr, i);
		if ((int)i == my_tid || !READ_ONCE(loc->retire_cnt))
			continue;
		if (atomic_xchg(&loc->reclaim_queued, 1))
			continue;

		/*
		 * Counted before the job can run: it counts itself out. A worker
		 * that is gone has no one to scan the buffer, so no request.
		 */
		atomic_inc(&ebr->limbo->reclaim_pending);
		if (!lsv_shard_wq_queue_work_by_id(i, &loc->reclaim_ctx)) {
			atomic_dec(&ebr->limbo->reclaim_pending);
			atomic_set(&loc->reclaim_queued, 0);
		}
	}
}

int ebr_limbo_alloc(struct lsv_ebr_state *ebr)
{
	struct lsv_ebr_limbo_state *limbo;
	unsigned int i;

	limbo = kvzalloc(struct_size(limbo, rec, ebr->nr_threads), GFP_KERNEL);
	if (!limbo)
		return -ENOMEM;

	limbo->ebr = ebr;
	for (i = 0; i < ebr->nr_threads; i++)
		lsv_shard_wq_init_work(&limbo->rec[i].reclaim_ctx, ebr_reclaim_request_run);

	ebr->limbo = limbo;
	return 0;
}

/*
 * Whether a reclaim request is queued on the worker this runs on. It cannot
 * be running: the worker is busy here, and runs it only after this returns.
 */
static bool ebr_own_request_queued(struct lsv_ebr_state *ebr)
{
	struct lsv_shard_wq_worker *worker = lsv_shard_wq_current_worker();
	unsigned int tid;

	if (!worker)
		return false;

	tid = lsv_shard_wq_worker_id(worker);
	return tid < ebr->nr_threads && atomic_read(&ebr_local(ebr, tid)->reclaim_queued);
}

/*
 * Reclaim requests still queued name the table, and no operation is left to
 * queue another one. Those of the other workers are waited for; the one
 * queued behind this very call would never come, so it is left the memory it
 * touches, and frees it once it runs.
 */
void ebr_limbo_destroy(struct lsv_ebr_state *ebr)
{
	struct lsv_ebr_limbo_state *limbo = ebr->limbo;
	bool own_request = ebr_own_request_queued(ebr);
	struct lsv_ebr_local_rec *loc;
	unsigned int i, j;

	while (atomic_read(&limbo->reclaim_pending) > own_request)
		cond_resched();

	for (i = 0; i < ebr->nr_threads; i++) {
		loc = ebr_local(ebr, i);
		for (j = 0; j < LSV_EBR_NR_EPOCHS; j++)
			ebr_limbo_free(ebr, loc, &loc->limbo[j]);
	}

	if (own_request)
		limbo->ebr = NULL;
	else
		kvfree(limbo);
	ebr->limbo = NULL;
}

size_t ebr_limbo_mem_usage(const struct lsv_ebr_state *ebr)
{
	return struct_size(ebr->limbo, rec, ebr->nr_threads);
}

#if IS_ENABLED(CONFIG_KUNIT)
/* Cells waiting in the limbo lists of |tid|. */
u32 ht_ebr_limbo_cnt(const struct lsv_ht *ht, unsigned int tid)
{
	return READ_ONCE(ht->ebr->limbo->rec[tid].retire_cnt);
}

/* Cells waiting in limbo, all participants together. */
u32 ht_ebr_limbo_total(const struct lsv_ht *ht)
{
	u32 total = 0;
	unsigned int i;

	for (i = 0; i < ht->ebr->nr_threads; i++)
		total += ht_ebr_limbo_cnt(ht, i);

	return total;
}

/* Reclaim requests queued on the workers of participants and not run yet. */
bool ht_kunit_reclaim_requests_pending(const struct lsv_ht *ht)
{
	return atomic_read(&ht->ebr->limbo->reclaim_pending);
}
#endif /* CONFIG_KUNIT */
