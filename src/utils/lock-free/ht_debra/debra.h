#ifndef _LSV_DEBRA_H
#define _LSV_DEBRA_H

#include <linux/atomic.h>
#include <linux/cache.h>
#include <linux/hash.h>
#include <linux/stddef.h>
#include <linux/types.h>

#include "ht_debra.h"
#include "atomic_ops.h"

#define LSV_DEBRA_NR_EPOCHS 3

/* lsv_debra_global_rec::state: (observed epoch << 1) | ACTIVE */
#define LSV_DEBRA_ACTIVE	1ULL
#define lsv_debra_state_epoch(s)	((s) >> 1)
#define lsv_debra_state_active(s)	(!!((s) & LSV_DEBRA_ACTIVE))
#define lsv_debra_state_make(e)		(((u64)(e) << 1) | LSV_DEBRA_ACTIVE)

// how many checks before advancing global epoch
#define LSV_CHECKS_THRESHOLD 1

// how many operations have to be done to scan another process' epoch
#define LSV_INCR_THRESHOLD 100

#define LSV_EBR_RETIRE_THRESHOLD	64

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
struct lsv_debra_global_rec {
	u64 state;
} ____cacheline_aligned;

struct lsv_debra_limbo {
	struct lsv_lf_list_node *head;
	u32 cnt;
};

struct lsv_debra_local_rec {
	struct lsv_debra_limbo limbo[LSV_DEBRA_NR_EPOCHS];
	/* index of the bag being filled; rotate-and-reclaim moves it on */
	u32 cur;
	/* participants checked for the current epoch, may pass nr_threads */
	unsigned int next_to_check;
	/* cells in all three limbo lists */
	u32 retire_cnt;
	/* leave_qstate calls since another announcement was read */
	u32 op_cnt;
} ____cacheline_aligned;

/* The limbo bags of every participant. */
struct lsv_debra_limbo_state {
	struct lsv_debra_state *debra;
	struct lsv_debra_local_rec rec[];
};

struct lsv_debra_state {
	/* the global epoch: written rarely, read by every operation */
	atomic64_t epoch ____cacheline_aligned;

	unsigned int nr_threads;
	struct lsv_debra_global_rec *global;

	struct lsv_debra_limbo_state *limbo;

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
static inline bool debra_table_filling(const struct lsv_ht *ht)
{
	return lsv_ht_entry_cnt(ht) * 100 >= ht->max_entry_cnt * LSV_HT_DEF_FILL_THRESHOLD;
}

/**
 * internals needed for the reclamation based on DEBRA scheme
 */

void debra_leave_qstate(struct lsv_debra_state *debra, int tid);

void debra_enter_qstate(struct lsv_debra_state *debra, int tid);

// retire
void debra_retire(struct lsv_debra_state *debra, int tid, struct lsv_lf_list_node *node);
void debra_retire_span(struct lsv_debra_state *debra, int tid,
		       struct lsv_lf_list_node *first, struct lsv_lf_list_node *last);

u32 debra_free_limbo(struct lsv_debra_state *debra, struct lsv_lf_list_node *node);

int debra_limbo_alloc(struct lsv_debra_state *debra);
void debra_limbo_destroy(struct lsv_debra_state *debra);
size_t debra_limbo_mem_usage(const struct lsv_debra_state *debra);

struct lsv_debra_state *debra_alloc(struct lsv_ht *ht);
void debra_destroy(struct lsv_debra_state *debra);
int debra_tid(const struct lsv_ht *ht);
u64 debra_epoch(const struct lsv_debra_state *debra);
size_t debra_mem_usage(const struct lsv_debra_state *debra);

#if IS_ENABLED(CONFIG_KUNIT)
void ht_kunit_cell_reached(struct lsv_lf_list_node *cell);
bool ht_debra_is_active(const struct lsv_ht *ht, unsigned int tid);
u32 ht_debra_limbo_cnt(const struct lsv_ht *ht, unsigned int tid);
u32 ht_debra_limbo_total(const struct lsv_ht *ht);
bool ht_kunit_reclaim_requests_pending(const struct lsv_ht *ht);
#else
static inline void ht_kunit_cell_reached(struct lsv_lf_list_node *cell) { }
#endif

#endif
