// SPDX-License-Identifier: GPL-2.0-only

/*
 * The part of [EBR] that does not depend on where the limbo lists live:
 * entering and leaving a critical region, the scan that decides whether the
 * epoch may move, participant ids. See ebr.h.
 */

#include <linux/minmax.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <kunit/static_stub.h>
#include <kunit/visibility.h>

#include "ebr.h"
#include "tests/include_static/__static_kunit_lsv_ht.h"

/*
 * [EBR] 5.2.3: entering a critical region. The participant announces itself
 * active in the epoch it has observed, and the announcement must be visible
 * before any load of the table: a participant that tries to advance the epoch
 * either sees it, or advances an epoch whose cells this one can no longer
 * reach.
 *
 * The epoch announced may already be stale by the time it is visible. That is
 * safe -- it only holds the epoch back -- but needless, so the epoch is read
 * again behind the barrier and the announcement repeated while it moves. The
 * epoch moves only when every active participant has observed it, so this
 * one, once announced, lets it move at most once more: two rounds at most.
 */
void ebr_enter(struct lsv_ebr_state *ebr, int tid)
{
	struct lsv_ebr_global_rec *rec = &ebr->global[tid];
	u64 seen;
	u64 e;

	e = atomic64_read(&ebr->epoch);
	for (;;) {
		WRITE_ONCE(rec->state, lsv_ebr_state_make(e));
		/* pairs with ebr_all_observed(): announce before loading the table */
		smp_mb();

		seen = atomic64_read(&ebr->epoch);
		if (seen == e)
			break;
		e = seen;
	}
}

/*
 * Leaving the critical region: every load of the table the region made is
 * ordered before the record goes quiescent, so that nothing it read can be
 * freed under it.
 */
void ebr_exit(struct lsv_ebr_state *ebr, int tid)
{
	/* pairs with the barrier before a free: the region's loads come first */
	smp_store_release(&ebr->global[tid].state, 0);
}

/*
 * [EBR]: whether every participant in a critical region has observed |e|,
 * an epoch the caller has read. Participants outside of any critical region
 * do not count (footnote 1 of 5.2.3).
 *
 * The full barrier between reading the epoch and reading the records pairs
 * with the one in ebr_enter(): a participant that announced itself before a
 * cell was retired in e is seen here, still active in e or before.
 */
bool ebr_all_observed(struct lsv_ebr_state *ebr, u64 e)
{
	unsigned int i;
	u64 state;

	/* pairs with ebr_enter(): read the epoch before the records */
	smp_mb();

	for (i = 0; i < ebr->nr_threads; i++) {
		state = READ_ONCE(ebr->global[i].state);
		if (lsv_ebr_state_active(state) && lsv_ebr_state_epoch(state) != e)
			return false;
	}

	ht_ebr_scanned(ebr, e);
	return true;
}

/*
 * Cells go back to the ops that made them, once each: an implementation that
 * counts the cells pointing at a value is told about every one of them.
 */
u32 ebr_chain_free(struct lsv_ebr_state *ebr, struct lsv_lf_list_node *node)
{
	struct lsv_lf_list_node *next;
	u32 freed = 0;

	for (; node; node = next) {
		next = node->removed_link;
		ebr->ops->node_dealloc(node, ebr->ctx);
		freed++;
	}

	return freed;
}

#if IS_ENABLED(CONFIG_KUNIT)
/*
 * Called on every cell any walk steps on, from any participant: KUnit checks
 * there that no walk ever reaches a cell the table has given back, and parks
 * a walk inside its critical region. A plain pointer and not a static stub,
 * whose lookup would serialize the walks of a stress test on a lock. The name
 * is the one of the DtA table, so that the generic suite runs on either.
 */
void (*g_lsv_ht_kunit_cell_check)(struct lsv_lf_list_node *cell);

void ht_kunit_cell_reached(struct lsv_lf_list_node *cell)
{
	void (*check)(struct lsv_lf_list_node *cell) = READ_ONCE(g_lsv_ht_kunit_cell_check);

	if (check)
		check(cell);
	KUNIT_STATIC_STUB_REDIRECT(ht_kunit_cell_reached, cell);
}

/* Called once a scan has found that everybody observed |e|, before anything moves. */
void (*g_lsv_ht_ebr_scanned)(struct lsv_ht *ht, u64 e);

void ht_ebr_scanned(struct lsv_ebr_state *ebr, u64 e)
{
	void (*hook)(struct lsv_ht *ht, u64 e) = READ_ONCE(g_lsv_ht_ebr_scanned);

	if (hook)
		hook(ebr->ht, e);
}
#endif

struct lsv_ebr_state *ebr_alloc(struct lsv_ht *ht)
{
	struct lsv_ebr_state *ebr;

	ebr = kzalloc_obj(*ebr);
	if (!ebr)
		return NULL;

	ebr->nr_threads = max_t(unsigned int, lsv_shard_wq_worker_id_limit(), 1);
	ebr->ht = ht;
	ebr->ops = ht->ops;
	ebr->ctx = ht->ctx;

	/* records start quiescent (state 0), and the epoch at 0 */
	ebr->global = kvzalloc_objs(*ebr->global, ebr->nr_threads);
	if (!ebr->global)
		goto err_free;

	if (ebr_limbo_alloc(ebr))
		goto err_free;

	return ebr;

err_free:
	kvfree(ebr->global);
	kfree(ebr);
	return NULL;
}

void ebr_destroy(struct lsv_ebr_state *ebr)
{
	ebr_limbo_destroy(ebr);
	kvfree(ebr->global);
	kfree(ebr);
}

int ebr_tid(const struct lsv_ht *ht)
{
	struct lsv_shard_wq_worker *worker = lsv_shard_wq_current_worker();
	unsigned int id;

	if (WARN_ON_ONCE(!worker))
		return -1;

	id = lsv_shard_wq_worker_id(worker);
	if (unlikely(id >= ht->ebr->nr_threads)) {
		/*
		 * The table sized its records when it was created, so a worker
		 * of a queue created after it has no place among the
		 * participants and every operation of it fails.
		 */
		pr_warn_ratelimited("lsv_ht: worker %u is not a participant of a table sized for %u\n",
				    id, ht->ebr->nr_threads);
		return -1;
	}

	return id;
}

u64 ebr_epoch(const struct lsv_ebr_state *ebr)
{
	return atomic64_read(&ebr->epoch);
}

size_t ebr_mem_usage(const struct lsv_ebr_state *ebr)
{
	return sizeof(*ebr) + ebr->nr_threads * sizeof(*ebr->global) + ebr_limbo_mem_usage(ebr);
}

#if IS_ENABLED(CONFIG_KUNIT)
/* Whether |tid| is inside a table operation. */
bool ht_ebr_is_active(const struct lsv_ht *ht, unsigned int tid)
{
	return lsv_ebr_state_active(READ_ONCE(ht->ebr->global[tid].state));
}
#endif /* CONFIG_KUNIT */
