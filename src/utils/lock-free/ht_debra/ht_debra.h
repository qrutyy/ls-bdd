#ifndef _LSV_HT_DEBRA_H
#define _LSV_HT_DEBRA_H

#include <linux/atomic.h>
#include <linux/limits.h>
#include <linux/types.h>

/**
 * definitions and api needed for hashtable with reclamation based on DEBRA
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
	/* low bit: deleted (Harris) */
	struct lsv_lf_list_node *next;
	/* limbo list link, see ebr_local.c and ebr_global.c */
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
	 * Retired cells wait in limbo until two epochs have passed. A
	 * participant turned away here advances the epoch as far as it can and
	 * frees what that lets it free: with ht=ebr its own limbo lists only,
	 * and it also asks every other participant with cells in limbo to do
	 * the same, on its own worker; with ht=ebr+ the shared lists, whoever
	 * retired the cells. A bounded pool must carry the entries and the
	 * cells retired during the last two epochs, plus whatever a stalled
	 * participant keeps from reclamation.
	 */
	lsv_lf_list_node_alloc_op_t node_alloc;
	/* Unused by this table: kept so that both tables take the same ops. */
	lsv_lf_list_node_copy_op_t node_copy;
	/*
	 * Gives back a cell; the value is not its to release. A cell of
	 * node_alloc() that an insertion gives back unpublished had its value
	 * made with an initial reference for the caller that never gets it:
	 * with value_put_new, the table drops that reference first.
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
 * the buckets have in common, the tail sentinel included.
 */
struct lsv_lf_list {
	struct lsv_lf_list_node head;
};

struct lsv_debra_state;

struct lsv_ht {
	/* |bucket_cnt| buckets */
	struct lsv_lf_list *buckets;
	/* where every bucket ends; never linked behind or marked */
	struct lsv_lf_list_node tail;

	const struct lsv_lf_list_ops *ops;
	void *ctx;

	struct lsv_debra_state *debra;

	u64 bucket_cnt;
	u64 bucket_bits;

	atomic64_t entry_cnt;
	u64 max_entry_cnt;
};

/*
 * The cell linked after |cell|, deleted tag stripped. For walking a table no
 * operation runs on any more, such as right before lsv_ht_free().
 */
static inline struct lsv_lf_list_node *lsv_ht_cell_next(const struct lsv_lf_list_node *cell)
{
	return (struct lsv_lf_list_node *)((uintptr_t)READ_ONCE(cell->next) & ~0x1UL);
}

static inline u64 lsv_ht_entry_cnt(const struct lsv_ht *ht)
{
	return atomic64_read(&ht->entry_cnt);
}

static inline bool lsv_ht_is_full(const struct lsv_ht *ht)
{
	return lsv_ht_entry_cnt(ht) >= ht->max_entry_cnt;
}

/* Returns an ERR_PTR: -EINVAL on inconsistent ops, -ENOMEM on allocation failure. */
struct lsv_ht *lsv_ht_alloc_and_init(u64 size, void *ctx, const struct lsv_lf_list_ops *ops);
/*
 * Once no operation runs on the table, from any thread, a participant's
 * worker included. With ht=ebr it waits for the reclaim requests still
 * queued on the workers of the other participants: their queues must keep
 * running until it returns, and none of them may be waiting for the caller
 * meanwhile. A request queued on the worker it runs on is not waited for; it
 * frees the last of the table once it runs, so that queue must not be
 * destroyed before it has run. With ht=ebr+ nothing is ever queued.
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
 * while its value has no reference left. Without value_get the key is removed
 * whatever it holds. True when this call unpublished the key. See ht_dta.h for
 * the full contract, which this table shares.
 */
bool lsv_ht_remove(struct lsv_ht *ht, u64 key);

/* The global epoch, a diagnostic: how far reclamation has got. */
u64 lsv_ht_epoch(const struct lsv_ht *ht);

bool lsv_ht_is_empty(struct lsv_ht *ht);
size_t lsv_ht_mem_usage(struct lsv_ht *ht, size_t node_size);

#endif /* _LSV_HT_DEBRA_H */
