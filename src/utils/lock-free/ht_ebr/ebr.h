/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef _LSV_EBR_H
#define _LSV_EBR_H

/*
 * Internals shared by the table (ht_ebr.c), the common part of the
 * reclamation (ebr.c) and its two back-ends. Not for users of the table: see
 * ht_ebr.h.
 *
 * Epoch-based reclamation as described by K. Fraser, "Practical lock-freedom",
 * PhD thesis, University of Cambridge, 2004 (UCAM-CL-TR-579), section 5.2.3;
 * referred to as [EBR] below.
 *
 *   - A global epoch counter, |epoch|.
 *   - Every participant announces, on entering a critical region (one table
 *     operation), that it is active and which global epoch it has observed.
 *   - A cell unlinked from a list goes to the limbo list of the epoch its
 *     retirer observes right after the unlink.
 *   - The global epoch may be advanced from e to e + 1 only once every
 *     participant in a critical region has observed e.
 *   - A cell retired in epoch r cannot be reached by anybody once the global
 *     epoch is r + 2: every critical region that could still hold it began
 *     in epoch r or earlier, and such a region keeps the epoch below r + 2.
 *     Three limbo lists, indexed by epoch mod 3, are enough.
 *
 * Where the limbo lists live is what the two back-ends differ in; everything
 * above is common to both (see docs/ebr.md for the comparison):
 *
 *   ebr_local.c  (ht=ebr)   three limbo lists per participant, each freed by
 *                            its owner, as most implementations of EBR do.
 *   ebr_global.c (ht=ebr+)  three limbo lists per table, shared by all
 *                            participants and freed by whoever advances the
 *                            epoch. Cell placement of the text of [EBR].
 *
 * [EBR] is blocking with respect to reclamation: a participant that stalls
 * inside an operation keeps the global epoch where it is, and nobody frees
 * anything until it moves on. That is the price of its cheap read side, and
 * what the DtA table (ht_dta) pays a recovery protocol to avoid.
 */

#include <linux/atomic.h>
#include <linux/cache.h>
#include <linux/hash.h>
#include <linux/stddef.h>
#include <linux/types.h>

#include "ht_ebr.h"
#include "core/wq/wq.h"

#define LSV_EBR_NR_EPOCHS	3

/*
 * [EBR] tries to advance the epoch "probabilistically", now and then on
 * entering a critical region, as a scan of every participant is not free.
 * Both back-ends try every LSV_EBR_ADVANCE_FREQ operations of a participant
 * instead, at the end of the operation, outside of the critical region, so
 * that the participant that scans never stands in its own way.
 */
#define LSV_EBR_ADVANCE_FREQ		32
/* ... and early, once a participant has retired this many cells */
#define LSV_EBR_RETIRE_THRESHOLD	64

/* lsv_ebr_global_rec::state: (observed epoch << 1) | ACTIVE */
#define LSV_EBR_ACTIVE		1ULL
#define lsv_ebr_state_epoch(s)	((s) >> 1)
#define lsv_ebr_state_active(s)	(!!((s) & LSV_EBR_ACTIVE))
#define lsv_ebr_state_make(e)	(((u64)(e) << 1) | LSV_EBR_ACTIVE)

/* Tag in lsv_lf_list_node::next: 0x1 marks the cell deleted (Harris). */
#define lsv_ht_lcas(ptr, old, new) \
	((__typeof__(*(ptr)))cmpxchg((ptr), (old), (new)))

#define lsv_ht_mark_node(x)	((struct lsv_lf_list_node *)((uintptr_t)(x) | 0x1UL))
#define lsv_ht_has_mark(x)	(!!((uintptr_t)(x) & 0x1UL))
#define lsv_ht_strip_mark(x)	((struct lsv_lf_list_node *)((uintptr_t)(x) & ~0x1UL))

#define LSV_HT_MAX_LOOKUP_RETRIES	100
#define LSV_HT_DEF_BUCKET_LOAD_FACTOR	10
#define LSV_HT_DEF_FILL_THRESHOLD	80

/*
 * Per-participant record that every participant reads when it tries to
 * advance the epoch, and that only its owner writes. A record takes a cache
 * line of its own: two sharing one would have each owner's entry invalidate
 * the other's line.
 */
struct lsv_ebr_global_rec {
	u64 state;
} ____cacheline_aligned;

/* The limbo lists and whatever else a back-end keeps: ebr_local.c, ebr_global.c. */
struct lsv_ebr_limbo_state;

struct lsv_ebr_state {
	/* the global epoch: written rarely, read by every operation */
	atomic64_t epoch ____cacheline_aligned;

	unsigned int nr_threads;
	struct lsv_ebr_global_rec *global;

	struct lsv_ebr_limbo_state *limbo;

	/* the table this state reclaims for */
	struct lsv_ht *ht;
	const struct lsv_lf_list_ops *ops;
	void *ctx;
};

static inline struct lsv_lf_list *ht_bucket(const struct lsv_ht *ht, u64 key)
{
	return &ht->buckets[hash_64(key, ht->bucket_bits)];
}

/* Whether the table is full enough for reclamation to hurry. */
static inline bool ebr_table_filling(const struct lsv_ht *ht)
{
	return lsv_ht_entry_cnt(ht) * 100 >= ht->max_entry_cnt * LSV_HT_DEF_FILL_THRESHOLD;
}

/* ebr.c: common to both back-ends */
void ebr_enter(struct lsv_ebr_state *ebr, int tid);
void ebr_exit(struct lsv_ebr_state *ebr, int tid);
bool ebr_all_observed(struct lsv_ebr_state *ebr, u64 e);
struct lsv_ebr_state *ebr_alloc(struct lsv_ht *ht);
void ebr_destroy(struct lsv_ebr_state *ebr);
int ebr_tid(const struct lsv_ht *ht);
u64 ebr_epoch(const struct lsv_ebr_state *ebr);
size_t ebr_mem_usage(const struct lsv_ebr_state *ebr);

/*
 * The back-end: exactly one of ebr_local.c and ebr_global.c is built in.
 *
 * ebr_limbo_alloc()/ebr_limbo_destroy() make and unmake ebr->limbo; destroy
 * gives every cell still in limbo back, and may wait for work that the
 * back-end has queued on the workers of participants.
 *
 * ebr_retire_span() is called inside the critical region of |tid|, right
 * after its CAS unlinked the cells. ebr_op_end() leaves that region and
 * reclaims what the back-end lets |tid| reclaim; it returns how many cells
 * it gave back. ebr_request_reclaim() follows an operation that found no
 * cell, outside of any critical region of |tid|.
 */
int ebr_limbo_alloc(struct lsv_ebr_state *ebr);
void ebr_limbo_destroy(struct lsv_ebr_state *ebr);
void ebr_retire_span(struct lsv_ebr_state *ebr, int tid,
		     struct lsv_lf_list_node *first, struct lsv_lf_list_node *last);
u32 ebr_op_end(struct lsv_ht *ht, int tid, bool no_cell);
void ebr_request_reclaim(struct lsv_ebr_state *ebr, int my_tid);
size_t ebr_limbo_mem_usage(const struct lsv_ebr_state *ebr);

/* Gives back every cell of a chain linked through removed_link. */
u32 ebr_chain_free(struct lsv_ebr_state *ebr, struct lsv_lf_list_node *node);

/* a walk has stepped onto |cell|; a successful scan of epoch |e| */
#if IS_ENABLED(CONFIG_KUNIT)
void ht_kunit_cell_reached(struct lsv_lf_list_node *cell);
void ht_ebr_scanned(struct lsv_ebr_state *ebr, u64 e);
#else
static inline void ht_kunit_cell_reached(struct lsv_lf_list_node *cell) { }
static inline void ht_ebr_scanned(struct lsv_ebr_state *ebr, u64 e) { }
#endif

#endif /* _LSV_EBR_H */
