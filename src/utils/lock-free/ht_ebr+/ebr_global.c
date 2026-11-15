// SPDX-License-Identifier: GPL-2.0-only

/*
 * ht=ebr+: [EBR] with three limbo lists for the whole table, as the text of
 * 5.2.3 has them.
 *
 * Every participant prepends what it retires to the shared list of the epoch
 * it observes, with a compare-and-swap. Whoever advances the epoch takes the
 * oldest list and frees it: "the process prepends the contents of the oldest
 * limbo list to the free list and then increments the epoch count". Nobody
 * owns any garbage, so nobody has to be asked to free it: any participant
 * that advances frees everybody's.
 *
 * Which list is "the oldest" follows from what epoch a cell is retired in.
 * [EBR] files it under the epoch its retirer observed when the operation
 * began, so a list may still be filled by a laggard one epoch after its own:
 * the move from e to e + 1 may free the list of e - 2 only, and a cell waits
 * for three advances. Here, as in ebr_local.c, a cell is filed under the epoch
 * read right after the unlink, which no reader of a later epoch can reach the
 * cell in: the move from e to e + 1 frees the list of e - 1, and a cell waits
 * for two advances in both back-ends.
 *
 * What the text leaves open is two participants advancing at once. Both scan,
 * both find everybody in e, both take "the oldest list" -- but the second one
 * may take it after the epoch has moved on twice, when that list is the one
 * the current epoch fills, and free its cells under the readers of the
 * current epoch. Advancing is therefore claimed: one participant at a time
 * detaches a list and moves the epoch, after checking that the epoch is still
 * the one it scanned. Retiring and reading never wait for the claim; only a
 * second advancer gives up, as it would on a lost compare-and-swap of the
 * epoch. See docs/ebr.md.
 */

#include <linux/minmax.h>
#include <linux/overflow.h>
#include <linux/preempt.h>
#include <linux/slab.h>

#include "utils/lock-free/ht_ebr/ebr.h"
#include "utils/lock-free/ht_ebr/tests/include_static/__static_kunit_lsv_ht.h"

/* A shared limbo list: a stack of cells linked through removed_link. */
struct lsv_ebr_shared_limbo {
	struct lsv_lf_list_node *head;
} ____cacheline_aligned;

/* Per-participant state, its owner's alone: what paces its attempts to advance. */
struct lsv_ebr_pacing_rec {
	/* operations ended */
	u32 op_cnt;
	/* cells retired since the last attempt to advance */
	u32 retired;
} ____cacheline_aligned;

struct lsv_ebr_limbo_state {
	/* list i holds the cells retired in epochs e with e mod 3 == i */
	struct lsv_ebr_shared_limbo list[LSV_EBR_NR_EPOCHS];
	/* the epoch is being advanced, see ebr_try_advance() */
	atomic_t advancing ____cacheline_aligned;
	struct lsv_ebr_pacing_rec rec[];
};

/*
 * [EBR]: once every participant in a critical region has observed e, frees
 * the list of e - 1 and moves the epoch to e + 1. Returns whether it did, and
 * adds what it freed to |freed|.
 *
 * Nobody fills the list of e - 1 any more: a participant that would has to be
 * in a critical region since e - 1 or before, and the scan found none. The
 * next to fill it is the epoch e + 2, after another advance.
 *
 * Only the participant that holds |advancing| detaches a list or writes the
 * epoch, and only while the epoch is still the one it scanned; the claim is
 * held across two stores and with preemption off, so that a preempted
 * advancer holds nobody else up. A scan that loses the claim is a scan that
 * lost the race, as with a compare-and-swap of the epoch.
 */
static bool ebr_try_advance(struct lsv_ebr_state *ebr, u32 *freed)
{
	struct lsv_ebr_limbo_state *limbo = ebr->limbo;
	struct lsv_lf_list_node *oldest;
	u64 e = atomic64_read(&ebr->epoch);

	if (!ebr_all_observed(ebr, e))
		return false;

	preempt_disable();
	/* a full barrier on success: the scan comes before the frees below */
	if (atomic_cmpxchg(&limbo->advancing, 0, 1)) {
		preempt_enable();
		return false;
	}

	if (atomic64_read(&ebr->epoch) != e) {
		atomic_set_release(&limbo->advancing, 0);
		preempt_enable();
		return false;
	}

	/* e - 1 mod 3, which is e + 2 mod 3: e never wraps, being a u64 */
	oldest = xchg(&limbo->list[(e + 2) % LSV_EBR_NR_EPOCHS].head, NULL);
	/* pairs with ebr_retire_span(): the list is gone before the epoch moves on */
	atomic64_set_release(&ebr->epoch, e + 1);
	/* the next advancer finds the epoch at e + 1 */
	atomic_set_release(&limbo->advancing, 0);
	preempt_enable();

	*freed += ebr_chain_free(ebr, oldest);
	return true;
}

/*
 * [EBR]: cells unlinked from a list go to the limbo list of the epoch their
 * retirer observes after the unlink. The unlinking CAS is a full barrier, so
 * the epoch read here is no older than the unlink.
 *
 * The span is linked into a chain first and prepended with a single
 * compare-and-swap. The list it goes to cannot be detached meanwhile: that
 * happens when the epoch moves from e + 1 to e + 2, and this participant, in
 * its critical region since some epoch no newer than e, keeps it at e + 1 at
 * most. For the same reason the head cannot come back to the same value
 * between the load and the compare-and-swap: no ABA.
 */
void ebr_retire_span(struct lsv_ebr_state *ebr, int tid,
		     struct lsv_lf_list_node *first, struct lsv_lf_list_node *last)
{
	/* pairs with ebr_try_advance(): what an advance to e detached is gone */
	u64 e = atomic64_read_acquire(&ebr->epoch);
	struct lsv_lf_list_node **head = &ebr->limbo->list[e % LSV_EBR_NR_EPOCHS].head;
	struct lsv_lf_list_node *chain = lsv_ht_strip_mark(first);
	struct lsv_lf_list_node *node = chain;
	struct lsv_lf_list_node *next;
	struct lsv_lf_list_node *old;
	u32 cnt = 1;

	for (;;) {
		next = lsv_ht_strip_mark(READ_ONCE(node->next));
		if (next == last)
			break;
		node->removed_link = next;
		node = next;
		cnt++;
	}

	old = READ_ONCE(*head);
	do {
		node->removed_link = old;
	} while (!try_cmpxchg(head, &old, chain));

	ebr->limbo->rec[tid].retired += cnt;
}

/*
 * Outside of any critical region: moves the epoch on as far as the others let
 * it, twice at most -- what a cell retired in the current epoch needs.
 */
static u32 ebr_reclaim_quiescent(struct lsv_ebr_state *ebr)
{
	u32 freed = 0;

	if (ebr_try_advance(ebr, &freed))
		ebr_try_advance(ebr, &freed);

	return freed;
}

/*
 * Paced as in ebr_local.c, but a participant that retired cells has nothing
 * of its own to check: they are freed by whichever advance comes two epochs
 * later, its own or not.
 */
u32 ebr_op_end(struct lsv_ht *ht, int tid, bool no_cell)
{
	struct lsv_ebr_state *ebr = ht->ebr;
	struct lsv_ebr_pacing_rec *loc = &ebr->limbo->rec[tid];
	bool paced;

	ebr_exit(ebr, tid);
	paced = ++loc->op_cnt % LSV_EBR_ADVANCE_FREQ == 0;

	if (no_cell || paced || loc->retired >= LSV_EBR_RETIRE_THRESHOLD ||
	    (loc->retired && ebr_table_filling(ht))) {
		loc->retired = 0;
		return ebr_reclaim_quiescent(ebr);
	}

	return 0;
}

/*
 * Nothing to ask for: the participant that ran out of cells has already
 * advanced the epoch as far as it could, and with it freed whatever any
 * participant had retired long enough ago.
 */
void ebr_request_reclaim(struct lsv_ebr_state *ebr, int my_tid)
{
}

int ebr_limbo_alloc(struct lsv_ebr_state *ebr)
{
	struct lsv_ebr_limbo_state *limbo;

	limbo = kvzalloc(struct_size(limbo, rec, ebr->nr_threads), GFP_KERNEL);
	if (!limbo)
		return -ENOMEM;

	ebr->limbo = limbo;

	return 0;
}

/* Nothing runs on the table any more, and nothing was queued on its behalf. */
void ebr_limbo_destroy(struct lsv_ebr_state *ebr)
{
	struct lsv_ebr_limbo_state *limbo = ebr->limbo;
	unsigned int i;

	for (i = 0; i < LSV_EBR_NR_EPOCHS; i++)
		ebr_chain_free(ebr, limbo->list[i].head);

	kvfree(limbo);
	ebr->limbo = NULL;
}

size_t ebr_limbo_mem_usage(const struct lsv_ebr_state *ebr)
{
	return struct_size(ebr->limbo, rec, ebr->nr_threads);
}

#if IS_ENABLED(CONFIG_KUNIT)
/* Cells waiting in limbo. Only while no operation runs: the lists are walked. */
u32 ht_ebr_limbo_total(const struct lsv_ht *ht)
{
	struct lsv_lf_list_node *node;
	u32 total = 0;
	unsigned int i;

	for (i = 0; i < LSV_EBR_NR_EPOCHS; i++)
		for (node = READ_ONCE(ht->ebr->limbo->list[i].head); node;
		     node = node->removed_link)
			total++;

	return total;
}

/* No reclaim request is ever queued: see ebr_request_reclaim(). */
bool ht_kunit_reclaim_requests_pending(const struct lsv_ht *ht)
{
	return false;
}
#endif /* CONFIG_KUNIT */
