
#ifndef _LSV_HT_H
#define _LSV_HT_H

#include <linux/atomic.h>
#include <linux/limits.h>
#include <linux/types.h>

/*
 * Concurrent hash index: every bucket is a Harris non-blocking sorted list,
 * and list cells are reclaimed with Drop the Anchor (Braginsky, Kogan,
 * Petrank, SPAA'13; referred to as [DtA] below).
 *
 * Link to paper: https://dl.acm.org/doi/10.1145/2486159.2486184
 *
 * Participants are lsv_wq workers: [DtA] assumes a fixed set of threads with
 * ids 0..n-1, and the worker id plays that role (lsv_worker_id()). Every
 * lsv_ht_* operation must run on a worker created before the table; any other
 * caller fails the operation. Callbacks in lsv_lf_list_ops must not call back
 * into the same table.
 *
 * Lifetimes: a cell is a pure list element that owns neither its key nor its
 * value. A value must outlive every cell that points at it. Recovery of a
 * stalled participant replaces cells with copies that point at the same
 * values, so a value can be reachable through several cells for a while, and
 * a cell must never be used after the call that found it has returned.
 *
 * A cell gets its value once and never changes it: node_alloc() with a key of
 * its own makes a cell for a new value, node_copy() makes one for the value of
 * an existing cell, and node_dealloc() gives the cell back. The table calls
 * each of them once per cell and never writes a cell's value itself, so an
 * implementation that counts the cells pointing at a value is told about every
 * one of them. It must take no reference on the value in node_copy(), and
 * release none in node_dealloc(): a copy and its source outlive each other in
 * no fixed order, and the two would not balance.
 */

/*
 * Tags in lsv_lf_list_node::next: 0x1 marks the cell deleted (Harris), 0x2
 * marks it frozen ([DtA] 4.2). Both must be stripped before dereferencing.
 */
#define lsv_ht_lcas(ptr, old, new) \
	((__typeof__(*(ptr)))lsvo_cmpxchg_u64((u64 *)(ptr), (u64)(old), (u64)(new)))

#define lsv_ht_mark_node(x)	((struct lsv_lf_list_node *)((uintptr_t)(x) | 0x1UL))
#define lsv_ht_has_mark(x)	(!!((uintptr_t)(x) & 0x1UL))
#define lsv_ht_freeze_node(x)	((struct lsv_lf_list_node *)((uintptr_t)(x) | 0x2UL))
#define lsv_ht_is_frozen(x)	(!!((uintptr_t)(x) & 0x2UL))
#define lsv_ht_strip_all(x)	((struct lsv_lf_list_node *)((uintptr_t)(x) & ~0x3UL))

#define lsv_HT_MAX_LOOKUP_RETRIES	100
#define lsv_HT_DEF_BUCKET_LOAD_FACTOR	10
#define lsv_HT_DEF_FILL_THRESHOLD	80

#define lsv_DTA_RETIRE_THRESHOLD	3	/* scan the buffer after N retires */
#define lsv_DTA_RECLAIM_BUDGET		32	/* cells examined by one scan */
#define lsv_DTA_RECOVERY_THRESHOLD	256	/* suspect a thread after N blocked scans */
/*
 * ... and only once it has held the same timestamp for this long. [DtA]
 * assumes recovery answers a thread failure, "typically very uncommon in real
 * systems"; a scan count alone also fires for a worker that was merely
 * preempted, and every recovery costs a restarted operation and cells that
 * stay out of reach until the recovery is over.
 */
#define lsv_DTA_RECOVERY_DELAY_MS	1000

/*
 * Low two bits of anchor_and_flags ([DtA] 3, Figure 1):
 *   running:   IDLE=0, STUCK=0
 *   idle:      IDLE=1, STUCK=0
 *   stuck:     IDLE=0, STUCK=1  (suspected by another thread)
 *   recovered: IDLE=1, STUCK=1
 */
#define lsv_DTA_STUCK_BIT	1ULL
#define lsv_DTA_IDLE_BIT	2ULL
#define lsv_DTA_FLAGS_MASK	(lsv_DTA_IDLE_BIT | lsv_DTA_STUCK_BIT)

#define lsv_dta_af_ptr(af)	((struct lsv_lf_list_node *)((af) & ~lsv_DTA_FLAGS_MASK))
#define lsv_dta_af_is_idle(af)	(!!((af) & lsv_DTA_IDLE_BIT))
#define lsv_dta_af_is_stuck(af)	(!!((af) & lsv_DTA_STUCK_BIT))
#define lsv_dta_af_is_suspected(af)	(!lsv_dta_af_is_idle(af) && lsv_dta_af_is_stuck(af))
#define lsv_dta_af_is_recovered(af)	(lsv_dta_af_is_idle(af) && lsv_dta_af_is_stuck(af))
#define lsv_dta_af_make(ptr, idle, stuck)				\
	((u64)(ptr) | ((idle) ? lsv_DTA_IDLE_BIT : 0ULL)		\
		    | ((stuck) ? lsv_DTA_STUCK_BIT : 0ULL))

/*
 * Per-thread global record ([DtA] Listing 1): readable and CAS-able by every
 * thread. timestamp and anchor_and_flags form the double-width CAS target.
 *
 * Every operation CASes its own record several times and reads everyone
 * else's, so a record takes a cache line of its own: two records sharing one
 * would have each owner's CASes invalidate the other's line.
 */
#define lsv_DTA_REC_ALIGN	64

struct lsv_dta_global_rec {
	u64 timestamp;
	u64 anchor_and_flags;
	u64 low_timestamp;
} __lsvo_aligned(lsv_DTA_REC_ALIGN);

lsvo_static_assert(lsvo_offsetof(struct lsv_dta_global_rec, timestamp) == 0);
lsvo_static_assert(lsvo_offsetof(struct lsv_dta_global_rec, anchor_and_flags) == 8);
lsvo_static_assert(sizeof(struct lsv_dta_global_rec) == lsv_DTA_REC_ALIGN);
/* dta_dwcas() lays the pair out as a little-endian u128 */
lsvo_static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__);

/* one cell for every entry of the table: a byte here is a byte per entry */
lsvo_static_assert(sizeof(struct lsv_lf_list_node) == 48);
/* and one bucket for every lsv_HT_DEF_BUCKET_LOAD_FACTOR of them */
lsvo_static_assert(sizeof(struct lsv_lf_list) == sizeof(struct lsv_lf_list_node));

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

#if lsvO_ABI_CONFIG_KUNIT
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

enum lsv_dta_find_rc {
	lsv_DTA_FIND_FOUND,
	lsv_DTA_FIND_DONE,
	lsv_DTA_FIND_ABORT,
	lsv_DTA_FIND_CNT,
};



#define LSV_HT_MIN_BUCKET_BITS 12
#define LSV_HT_MAX_BUCKET_BITS 20

#define LSV_HT_KEY_HEAD_SENTINEL ((u64)0)
#define LSV_HT_KEY_TAIL_SENTINEL U64_MAX

static inline bool lsv_ht_key_is_reserved(u64 key)
{
	return key == LSV_HT_KEY_HEAD_SENTINEL || key == LSV_HT_KEY_TAIL_SENTINEL;
}

struct lsv_ht;
struct lsv_lf_list;

struct lsv_lf_list_node {
	struct lsv_lf_list_node *next;
	struct lsv_lf_list_node *removed_link;

	/* set once when the cell is created, never changed */
	u64 key;
	void *value;
};

typedef struct lsv_lf_list_node *(*lsv_lf_list_node_alloc_op_t)(u64 key, void *ctx);
typedef struct lsv_lf_list_node *(*lsv_lf_list_node_copy_op_t)(struct lsv_lf_list_node *src,
							       void *ctx);
typedef void (*lsv_lf_list_node_dealloc_op_t)(struct lsv_lf_list_node *node, void *ctx);
typedef bool (*lsv_lf_list_value_get_op_t)(struct lsv_lf_list_node *node, void *ctx);
typedef bool (*lsv_lf_list_value_is_dead_op_t)(struct lsv_lf_list_node *node, void *ctx);
typedef void (*lsv_lf_list_value_put_new_op_t)(struct lsv_lf_list_node *node, void *ctx);

struct lsv_lf_list_ops {
	/*
	 * A cell for a new value. Never called with a reserved key: the
	 * sentinels are part of the table and never come from here.
	 *
	 * Cells retired by a participant wait in its own buffer until it
	 * reclaims them, which it does after a few retirements or when this
	 * turns it away. A participant turned away that gets nothing back
	 * itself also has every other one with a buffered cell scan its
	 * buffer, on its own worker: the insertions that come after find the
	 * cells even of a participant that no longer uses the table. A bounded
	 * pool must carry the entries, the cells a stalled participant keeps
	 * from reclamation, and the few each idle participant holds until
	 * such a request reaches it.
	 */
	lsv_lf_list_node_alloc_op_t node_alloc;
	/*
	 * A fresh cell carrying the key and the very same value as src. Called
	 * only to recover a stalled participant, and a recovery that cannot
	 * copy cannot finish: it must not depend on the pool of node_alloc()
	 * alone, see LSV_DTA_SPARE_CELLS.
	 */
	lsv_lf_list_node_copy_op_t node_copy;
	/*
	 * Gives back a cell of either kind; the value is not its to release.
	 * A cell of node_alloc() that an insertion gives back unpublished had
	 * its value made with an initial reference for the caller that never
	 * gets it: with value_put_new, the table drops that reference first.
	 */
	lsv_lf_list_node_dealloc_op_t node_dealloc;
	/*
	 * The value's references: all three, or none when values are not
	 * refcounted. value_get() takes one unless none is left, and a value
	 * that has run out never gets one back; value_is_dead() tells that it
	 * has run out. value_put_new() drops the initial reference of a value
	 * nobody else has seen, the last one it has.
	 */
	lsv_lf_list_value_get_op_t value_get;
	lsv_lf_list_value_is_dead_op_t value_is_dead;
	lsv_lf_list_value_put_new_op_t value_put_new;
};

/*
 * A bucket is its head sentinel and nothing else: the table keeps everything
 * the buckets have in common, the tail sentinel included, so that the first
 * cell of a bucket is a single load away from the bucket array.
 */
struct lsv_lf_list {
	struct lsv_lf_list_node head;
};

struct lsv_ht {
	/* |bucket_cnt| buckets */
	struct lsv_lf_list *buckets;
	/* where every bucket ends; never linked behind, marked or frozen */
	struct lsv_lf_list_node tail;

	const struct lsv_lf_list_ops *ops;
	void *ctx;

	struct lsv_dta_state *dta;

	u64 bucket_cnt;
	u64 bucket_bits;

	atomic64_t entry_cnt;
	u64 max_entry_cnt;
};

static inline u64 lsv_ht_entry_cnt(const struct lsv_ht *ht)
{
	return atomic64_read(&ht->entry_cnt);
}

static inline bool lsv_ht_is_full(const struct lsv_ht *ht)
{
	return lsv_ht_entry_cnt(ht) >= ht->max_entry_cnt;
}

/*
 * Returns an ERR_PTR: -EOPNOTSUPP when the platform lacks the double-width
 * compare-and-swap [DtA] relies on, -ENOMEM on allocation failure.
 */
struct lsv_ht *lsv_ht_alloc_and_init(u64 size, void *ctx, const struct lsv_lf_list_ops *ops);
/*
 * Once no operation runs on the table, from any thread, a participant's
 * worker included. It waits for the reclaim requests still queued on the
 * workers of the other participants: their queues must keep running until it
 * returns, and none of them may be waiting for the caller meanwhile. A request
 * queued on the worker it runs on is not waited for; it frees the last of the
 * table once it runs, so that queue must not be destroyed before it has run.
 */
void lsv_ht_free(struct lsv_ht *ht);

/*
 * lsv_ht_insert_or_inc_ref() returns the value pinned (its reference was
 * taken, or it is a new value whose initial reference belongs to the caller).
 * lsv_ht_find_value() takes no reference: its caller must keep the value
 * alive by other means. Both may fail spuriously and return NULL.
 *
 * A value whose reference count has reached zero is never handed out by either
 * of them, and its key is not held by it: the table unpublishes such a cell as
 * soon as an operation meets it, and the key can be inserted anew.
 */
void *lsv_ht_find_value(struct lsv_ht *ht, u64 key);
void *lsv_ht_insert_or_inc_ref(struct lsv_ht *ht, u64 key, bool *is_new);


/*
 * The remove that follows the last put of a value: unpublishes the key only
 * while its value has no reference left. A key whose value is referenced is
 * left alone, whichever value that is -- the one the caller has just released
 * may have been dropped from the table already, and the key inserted anew as
 * a value that is someone else's. Without value_get the key is removed
 * whatever it holds.
 *
 * True when this call unpublished the key. False means the key is not there
 * any more, or is there for a live value; it includes a spurious failure and
 * a cell the table dropped on behalf of a value that had already lost its
 * last reference. The caller must not read it as "the value is still mine".
 *
 * The value may be destroyed from another thread as soon as its last reference
 * is gone, which can happen before this call returns: whatever the caller
 * still needs from the value must be taken out of it beforehand.
 *
 * A removal that fails costs the table the place of that entry until the key
 * is asked for again: the first operation to meet the cell unpublishes it,
 * and nothing else goes looking. A key that is never asked for again keeps
 * its place for as long as the table lives.
 */
bool lsv_ht_remove(struct lsv_ht *ht, u64 key);


bool lsv_ht_is_empty(struct lsv_ht *ht);
size_t lsv_ht_mem_usage(struct lsv_ht *ht, size_t node_size);

#endif /* _LSV_HT_H */
