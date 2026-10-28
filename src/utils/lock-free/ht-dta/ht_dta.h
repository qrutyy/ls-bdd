
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
	/* two low bits: 0x1 deleted, 0x2 frozen */
	struct lsv_lf_list_node *next;
	/*
	 * A cell is first an unpublished recovery copy, if it is one at all, and
	 * is queued for reclamation only after it has been published and then
	 * unlinked, so the two never share a moment.
	 */
	union {
		/* reclamation buffer link, owned by the retiring participant */
		struct lsv_lf_list_node *removed_link;
		/* recovery copies until published: the frozen lsvginal replaced */
		struct lsv_lf_list_node *copy_src;
	};
	u64 insert_ts;
	/*
	 * Both halves of this word are the retiring participant's alone, like
	 * |removed_link|. A timestamp never reaches bit 63.
	 */
	u64 retire_ts : 63;

	/*
	 * Retired while a recovery could still depend on it ([DtA] 4.5), or a
	 * frozen lsvginal bypassed by a recovery. Such cells are held back
	 * until no thread is stuck or recovered any more.
	 */
	u64 retired_frozen : 1;

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

struct lsv_dta_state;

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

/*
 * The cell linked after |cell|, deleted and frozen tags stripped. For walking a
 * table no operation runs on any more, such as right before lsv_ht_free().
 */
static inline struct lsv_lf_list_node *lsv_ht_cell_next(const struct lsv_lf_list_node *cell)
{
	return (struct lsv_lf_list_node *)((uintptr_t)READ_ONCE(cell->next) & ~0x3UL);
}

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
 * Restore path: the caller owns |node|; the returned cell is compared against
 * it to tell insertion from an existing key and must not be dereferenced.
 * NULL is a third outcome, a spurious failure that inserted nothing.
 */
struct lsv_lf_list_node *lsv_ht_insert_node(struct lsv_ht *ht, u64 key,
					    struct lsv_lf_list_node *node);

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

/*
 * Recoveries completed on this table. Each one answers a participant that has
 * been holding an operation open for a while, and costs that participant a
 * restart and the table the cells of the range that was frozen for it. A
 * recovery counts once it is over, however many passes it took.
 */
u64 lsv_ht_recovery_cnt(const struct lsv_ht *ht);

bool lsv_ht_is_empty(struct lsv_ht *ht);
size_t lsv_ht_mem_usage(struct lsv_ht *ht, size_t node_size);

#endif /* _LSV_HT_H */
