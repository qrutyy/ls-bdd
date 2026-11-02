#ifndef _LSV_EBR_H
#define _LSV_EBR_H

/*
 * Internals shared by the reclamation (ebr.c) and the table (ht_ebr-.c),
 * which reach into each other's lists and records. Not for users of the
 * table: see ht_ebr-.h.
 */

#include <linux/atomic.h>
#include <linux/build_bug.h>
#include <linux/hash.h>
#include <linux/stddef.h>
#include <linux/types.h>

#include "ht_ebr-.h"
#include "utils/wq.h"

#define LSV_EBR_RETIRE_THRESHOLD	3	/* scan the buffer after N retires */
#define LSV_EBR_RECLAIM_BUDGET		32	/* cells freed by one scan at most */

/*
 * A participant's record is a single word: its timestamp, shifted left by one,
 * over the IDLE bit. One word is read and written whole, so a reader never
 * pairs the timestamp of one operation with the state of another.
 *
 * An idle participant keeps its last timestamp, so that the timestamps never
 * go back: ebr_calc_max_ts() reads idle records too.
 */
#define LSV_EBR_IDLE_BIT	1ULL

#define lsv_ebr_rec_ts(state)		((state) >> 1)
#define lsv_ebr_rec_is_idle(state)	(!!((state) & LSV_EBR_IDLE_BIT))
#define lsv_ebr_rec_make(ts, idle)	(((u64)(ts) << 1) | ((idle) ? LSV_EBR_IDLE_BIT : 0ULL))

/*
 * Tag in lsv_lf_list_node::next: 0x1 marks the cell deleted (Harris). It must
 * be stripped before dereferencing.
 */
#define lsv_ht_lcas(ptr, old, new) \
	((__typeof__(*(ptr)))cmpxchg((ptr), (old), (new)))

#define lsv_ht_mark_node(x)	((struct lsv_lf_list_node *)((uintptr_t)(x) | 0x1UL))
#define lsv_ht_has_mark(x)	(!!((uintptr_t)(x) & 0x1UL))
#define lsv_ht_strip_all(x)	((struct lsv_lf_list_node *)((uintptr_t)(x) & ~0x1UL))

#define LSV_HT_MAX_LOOKUP_RETRIES	100
#define LSV_HT_DEF_BUCKET_LOAD_FACTOR	10
#define LSV_HT_DEF_FILL_THRESHOLD	80

/*
 * Per-thread global record: written by its owner only, read by every thread.
 *
 * Every operation writes its own record twice and reads everyone else's, so a
 * record takes a cache line of its own: two records sharing one would have
 * each owner's stores invalidate the other's line.
 */
#define LSV_EBR_REC_ALIGN 64

struct lsv_ebr_global_rec {
	u64 state;
} __aligned(LSV_EBR_REC_ALIGN);

static_assert(sizeof(struct lsv_ebr_global_rec) == LSV_EBR_REC_ALIGN);

/*
 * Per-thread local record: private to its owner.
 *
 * The retire buffer is a queue in retirement order, and so in the order of the
 * retire timestamps: a participant's own stamps never go down. A scan frees
 * from the head and stops at the first cell still out of range.
 */
struct lsv_ebr_local_rec {
	struct lsv_lf_list_node *retire_head, *retire_tail;
	u32 retire_cnt;
	/* |reclaim_ctx| is queued on this participant's worker */
	atomic_t reclaim_queued;
	/*
	 * A reclamation of this buffer, asked for by a participant that ran
	 * out of cells: the buffer is its owner's alone, so the scan runs on
	 * the owner's worker, between two of its operations.
	 */
	struct lsv_wq_ctx reclaim_ctx;
};

/* The local records and the state they belong to, which a reclaim request needs. */
struct lsv_ebr_local_block {
	struct lsv_ebr_state *ebr;
	struct lsv_ebr_local_rec rec[];
};

struct lsv_ebr_state {
	unsigned int nr_threads;
	struct lsv_ebr_global_rec *global;
	struct lsv_ebr_local_rec *local;

	/* reclaim requests queued and not run yet; lsv_ht_free() waits for them */
	atomic_t reclaim_pending;

	/* the table this state reclaims for: its buckets and its entry count */
	struct lsv_ht *ht;
	/* the table's tail sentinel, compared against on every step of a walk */
	struct lsv_lf_list_node *tail;
	const struct lsv_lf_list_ops *ops;
	void *ctx;
};

static inline struct lsv_lf_list *ht_bucket(const struct lsv_ht *ht, u64 key)
{
	return &ht->buckets[hash_64(key, ht->bucket_bits)];
}

/* ebr.c */
u64 ebr_calc_max_ts(const struct lsv_ebr_state *ebr);
void ebr_begin_op(struct lsv_ebr_state *ebr, int tid);
void ebr_retire_span(struct lsv_ebr_state *ebr, int tid,
		     struct lsv_lf_list_node *first, struct lsv_lf_list_node *last);
u32 ebr_try_reclaim(struct lsv_ebr_state *ebr, int tid);
void ebr_request_reclaim(struct lsv_ebr_state *ebr, int my_tid);
struct lsv_ebr_state *ebr_alloc(struct lsv_ht *ht);
void ebr_free(struct lsv_ebr_state *ebr, bool own_request);
int ht_tid(const struct lsv_ht *ht);
void ht_op_end(struct lsv_ht *ht, int tid);
/* both: a traversal has stepped onto |cell| */
void ht_kunit_cell_reached(struct lsv_lf_list_node *cell);

#endif /* _LSV_EBR_H */
