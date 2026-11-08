#ifndef _LSV_DTA_H
#define _LSV_DTA_H

/*
 * Internals shared by the reclamation (dta.c) and the table (ht_dta.c), which
 * reach into each other's lists and records. Not for users of the table: see
 * ht_dta.h.
 */

#include <linux/atomic.h>
#include <linux/build_bug.h>
#include <linux/hash.h>
#include <linux/stddef.h>
#include <linux/types.h>

#include "ht_dta.h"
#include "utils/wq.h"

#define LSV_DTA_ANCHOR_THRESHOLD	5	/* drop an anchor every N live cells */

/*
 * Cells worth keeping per participant for node_copy(), out of reach of
 * ordinary inserts: enough for a range of a stalled participant that nothing
 * was inserted into. They are a fast path, not a bound.
 *
 * Recovery copies the live cells of the range it freezes, and freezing cannot
 * be undone, so a recovery that cannot copy cannot finish -- and until it
 * does, the participant it answers keeps the reclamation bounds of the whole
 * table where they are. No number of cells covers every range: besides the
 * anchor and the LSV_DTA_ANCHOR_THRESHOLD cells older than the stalled
 * operation, it holds every cell inserted into it while that operation
 * stalled. Nor do the cells a recovery has spent come back before it is over:
 * the lsvginals it replaced are what the stalled participant may still be
 * standing on. node_copy() must therefore fall back on an allocator the
 * table cannot fill up once the cells kept for it are gone, and may fail only
 * when that allocator does; a recovery it failed is tried again later.
 */
#define LSV_DTA_SPARE_CELLS		(2 * LSV_DTA_ANCHOR_THRESHOLD)

/*
 * Cells one pass of a recovery copies at most. A wider range is replaced a
 * pass at a time, and so is one whose copies node_copy() could not serve in
 * full, so that no range is too wide to recover.
 *
 * It bounds the copies of a pass, not its walks: every pass still walks the
 * whole range left frozen, and the bucket up to it, so replacing a range of N
 * cells walks about N * N / (2 * LSV_DTA_SPARE_LIMIT) of them. A pass that
 * resumes where the last one stopped is a follow-up.
 */
#define LSV_DTA_SPARE_LIMIT		(4 * LSV_DTA_SPARE_CELLS)

#define LSV_DTA_RETIRE_THRESHOLD	3	/* scan the buffer after N retires */
#define LSV_DTA_RECLAIM_BUDGET		32	/* cells examined by one scan */
#define LSV_DTA_RECOVERY_THRESHOLD	256	/* suspect a thread after N blocked scans */
/*
 * ... and only once it has held the same timestamp for this long. [DtA]
 * assumes recovery answers a thread failure, "typically very uncommon in real
 * systems"; a scan count alone also fires for a worker that was merely
 * preempted, and every recovery costs a restarted operation and cells that
 * stay out of reach until the recovery is over.
 */
#define LSV_DTA_RECOVERY_DELAY_MS	1000

/*
 * Low two bits of anchor_and_flags ([DtA] 3, Figure 1):
 *   running:   IDLE=0, STUCK=0
 *   idle:      IDLE=1, STUCK=0
 *   stuck:     IDLE=0, STUCK=1  (suspected by another thread)
 *   recovered: IDLE=1, STUCK=1
 */
#define LSV_DTA_STUCK_BIT	1ULL
#define LSV_DTA_IDLE_BIT	2ULL
#define LSV_DTA_FLAGS_MASK	(LSV_DTA_IDLE_BIT | LSV_DTA_STUCK_BIT)

#define lsv_dta_af_ptr(af)	((struct lsv_lf_list_node *)((af) & ~LSV_DTA_FLAGS_MASK))
#define lsv_dta_af_is_idle(af)	(!!((af) & LSV_DTA_IDLE_BIT))
#define lsv_dta_af_is_stuck(af)	(!!((af) & LSV_DTA_STUCK_BIT))
#define lsv_dta_af_is_suspected(af)	(!lsv_dta_af_is_idle(af) && lsv_dta_af_is_stuck(af))
#define lsv_dta_af_is_recovered(af)	(lsv_dta_af_is_idle(af) && lsv_dta_af_is_stuck(af))
#define lsv_dta_af_make(ptr, idle, stuck)				\
	((u64)(ptr) | ((idle) ? LSV_DTA_IDLE_BIT : 0ULL)		\
		    | ((stuck) ? LSV_DTA_STUCK_BIT : 0ULL))

/*
 * Tags in lsv_lf_list_node::next: 0x1 marks the cell deleted (Harris), 0x2
 * marks it frozen ([DtA] 4.2). Both must be stripped before dereferencing.
 */
#define lsv_ht_lcas(ptr, old, new) \
	((__typeof__(*(ptr)))cmpxchg((ptr), (old), (new)))

#define lsv_ht_mark_node(x)	((struct lsv_lf_list_node *)((uintptr_t)(x) | 0x1UL))
#define lsv_ht_has_mark(x)	(!!((uintptr_t)(x) & 0x1UL))
#define lsv_ht_freeze_node(x)	((struct lsv_lf_list_node *)((uintptr_t)(x) | 0x2UL))
#define lsv_ht_is_frozen(x)	(!!((uintptr_t)(x) & 0x2UL))
#define lsv_ht_strip_all(x)	((struct lsv_lf_list_node *)((uintptr_t)(x) & ~0x3UL))

#define LSV_HT_MAX_LOOKUP_RETRIES	100
#define LSV_HT_DEF_BUCKET_LOAD_FACTOR	10
#define LSV_HT_DEF_FILL_THRESHOLD	80

#define LSV_DTA_REC_ALIGN 64

/*
 * Per-thread global record ([DtA] Listing 1): readable and CAS-able by every
 * thread. timestamp and anchor_and_flags form the double-width CAS target.
 *
 * Every operation CASes its own record several times and reads everyone
 * else's, so a record takes a cache line of its own: two records sharing one
 * would have each owner's CASes invalidate the other's line.
 */
struct lsv_dta_global_rec {
	u64 timestamp;
	u64 anchor_and_flags;
	u64 low_timestamp;
} __aligned(LSV_DTA_REC_ALIGN);

static_assert(offsetof(struct lsv_dta_global_rec, timestamp) == 0);
static_assert(offsetof(struct lsv_dta_global_rec, anchor_and_flags) == 8);
static_assert(sizeof(struct lsv_dta_global_rec) == LSV_DTA_REC_ALIGN);
/* dta_dwcas() lays the pair out as a little-endian u128 */
static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__);

/*
 * Per-thread local record ([DtA] Listing 1): private to its owner.
 *
 * The retire buffer is a queue: cells are appended at the tail and examined
 * from the head, so one bounded pass always looks at the oldest retirements,
 * the ones whose timestamps are most likely to have come into range. Cells a
 * stuck or recovered thread may still reach wait in |frozen_head| instead of
 * being rescanned on every pass, and rejoin the queue once no thread is.
 */
struct lsv_dta_local_rec {
	struct lsv_lf_list_node *retire_head, *retire_tail;
	struct lsv_lf_list_node *frozen_head, *frozen_tail;
	u32 retire_cnt;
	u32 frozen_cnt;
	u64 min_timestamp;
	unsigned long min_timestamp_at;
	u32 min_timestamp_tid;
	u32 min_timestamp_cnt;
	/* where dta_help_recovery() looks for a suspected participant first */
	u32 help_next;
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
struct lsv_dta_local_block {
	struct lsv_dta_state *dta;
	struct lsv_dta_local_rec rec[];
};

struct lsv_dta_state {
	unsigned int nr_threads;
	struct lsv_dta_global_rec *global;
	struct lsv_dta_local_rec *local;

	/*
	 * [DtA] 4.5: a retire timestamp computed concurrently with a thread
	 * being marked stuck or recovered may exceed the recovery timestamp.
	 * Recoveries bracket those transitions so a retire can detect them.
	 */
	atomic_t recovery_active;
	atomic64_t recovery_seq;

	/* completed recoveries, a diagnostic: every one costs a restart */
	atomic64_t recovery_cnt;

	/* reclaim requests queued and not run yet; lsv_ht_free() waits for them */
	atomic_t reclaim_pending;

#if IS_ENABLED(CONFIG_KUNIT)
	/* passes given up on a range frozen for another participant */
	atomic64_t recovery_aborts;
	/* cells the walks of recoveries have stepped on */
	atomic64_t recovery_walked;
#endif

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

/* dta.c */
u64 dta_calc_max_ts(const struct lsv_dta_state *dta);
void dta_begin_op(struct lsv_dta_state *dta, int tid, struct lsv_lf_list_node *anchor);
bool dta_set_anchor(struct lsv_dta_state *dta, int tid, struct lsv_lf_list_node *new_anchor);
void dta_retire_span(struct lsv_dta_state *dta, int tid,
		     struct lsv_lf_list_node *first, struct lsv_lf_list_node *last);
void dta_help_recovery(struct lsv_dta_state *dta, int my_tid);
void dta_request_reclaim(struct lsv_dta_state *dta, int my_tid);
struct lsv_dta_state *dta_alloc(struct lsv_ht *ht);
void dta_free(struct lsv_dta_state *dta, bool own_request);
int ht_tid(const struct lsv_ht *ht);
u32 ht_op_end(struct lsv_ht *ht, int tid, bool no_cell);
/* both: a walk has stepped onto |cell| */
void ht_cell_reached(struct lsv_lf_list_node *cell);

#endif /* _LSV_DTA_H */
