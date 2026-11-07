#ifndef _LSV_HT_H
#define _LSV_HT_H

#include <linux/atomic.h>
#include <linux/limits.h>
#include <linux/types.h>

/*
 * Concurrent hash index: every bucket is a Harris non-blocking sorted list,
 * and list cells are reclaimed by timestamps, the way the fast path of Drop
 * the Anchor does it (Braginsky, Kogan, Petrank, SPAA'13; [DtA] below), with
 * neither its anchors nor the recovery they serve.
 *
 * Link to paper: https://dl.acm.org/doi/10.1145/2486159.2486184
 *
 * Every operation takes a timestamp above all those taken before it, and a
 * cell unlinked from a list is stamped above every timestamp taken by then.
 * The cell is given back once every participant still running has begun
 * after that stamp: none of them can reach it any more.
 *
 * Without the recovery of [DtA], a participant that stops in the middle of an
 * operation would hold back every cell retired after it began, for as long as
 * it stays there. So an operation runs with preemption off and never sleeps:
 * value_get() and value_is_dead() are called inside one and must not sleep
 * either, while node_alloc(), node_dealloc() and value_put_new() are only
 * called between operations, and may.
 *
 * Participants are lsv_wq workers: the table keeps a record per worker id,
 * 0..n-1 (lsv_worker_id()). Every lsv_ht_* operation must run on a worker
 * created before the table; any other caller fails the operation. Callbacks
 * in lsv_lf_list_ops must not call back into the same table.
 *
 * Lifetimes: a cell is a pure list element that owns neither its key nor its
 * value. A value must outlive every cell that points at it, and a cell must
 * never be used after the call that found it has returned.
 *
 * A cell gets its value once and never changes it: node_alloc() makes a cell
 * for a new value, and node_dealloc() gives the cell back. The table calls
 * each of them once per cell and never writes a cell's value itself.
 */

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
	/* low bit: 0x1 deleted */
	struct lsv_lf_list_node *next;
	/* reclamation buffer link, owned by the retiring participant */
	struct lsv_lf_list_node *removed_link;
	/* above every timestamp taken before the cell was unlinked */
	u64 retire_ts;

	/* set once when the cell is created, never changed */
	u64 key;
	void *value;
};

typedef struct lsv_lf_list_node *(*lsv_lf_list_node_alloc_op_t)(u64 key, void *ctx);
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
	 * pool must carry the entries, the cells retired while some
	 * participant stays in an operation, and the few each idle participant
	 * holds until such a request reaches it.
	 */
	lsv_lf_list_node_alloc_op_t node_alloc;
	/*
	 * Gives back a cell; the value is not its to release. A cell that an
	 * insertion gives back unpublished had its value made with an initial
	 * reference for the caller that never gets it: with value_put_new, the
	 * table drops that reference first.
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

struct lsv_ebr_state;

struct lsv_ht {
	/* |bucket_cnt| buckets */
	struct lsv_lf_list *buckets;
	/* where every bucket ends; never linked behind or marked */
	struct lsv_lf_list_node tail;

	const struct lsv_lf_list_ops *ops;
	void *ctx;

	struct lsv_ebr_state *ebr;

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

/* Returns an ERR_PTR: -EINVAL for inconsistent ops, -ENOMEM on allocation failure. */
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
