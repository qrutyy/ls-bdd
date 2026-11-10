#ifndef _LSV_KUNIT_HT_H
#define _LSV_KUNIT_HT_H

#include <kunit/test.h>
#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/list.h>
#include <linux/preempt.h>
#include <linux/prandom.h>
#include <linux/spinlock.h>
#include <linux/refcount.h>

#include "utils/wq.h"

/*
 * The fixture every lsv_ht implementation is tested with. The tests are
 * compiled against the implementation Kbuild picked (ht=...), which describes
 * itself in its tests/ht_impl_test.h: the headers of the table and of its
 * KUnit hooks, and the LSV_HT_* switches below.
 *
 * LSV_HT_IMPL_NAME		"dta", ...: the suites are named after it
 * LSV_HT_HAS_RECOVERY		the table recovers stalled participants, and
 *				has node_copy(), lsv_ht_insert_node() and
 *				lsv_ht_recovery_cnt() for it
 * LSV_HT_OPS_NONPREEMPTIBLE	an operation runs with preemption off, hooks
 *				inside it included
 * lsv_ht_test_stall_hook	the hook a traversal calls where a participant
 *				that stalls holds back the most
 * LSV_HT_TEST_STRESS_RESERVE	cells beyond the pool a roomy stress run gives
 *				node_copy(), and LSV_HT_TEST_TIGHT_RESERVE a
 *				tight one: 0 without recovery
 */
#include "ht_impl_test.h"

#ifndef LSV_HT_HAS_RECOVERY
#define LSV_HT_HAS_RECOVERY 0
#endif
#ifndef LSV_HT_OPS_NONPREEMPTIBLE
#define LSV_HT_OPS_NONPREEMPTIBLE 0
#endif

#define LSV_HT_TEST_WAIT_MS 60000
#define LSV_HT_TEST_WQ_CNT 2

/* Chains of the values each key stands for. */
#define LSV_HT_TEST_KEY_BITS 6
/* keys of the one bucket the stress cases share */
#define LSV_HT_TEST_STALL_KEYS 24

struct lsv_ht_test_ctx;
struct lsv_ht_test_job;

/*
 * A stub of a hook inside an operation runs with preemption off when the whole
 * operation does. A test that parks a participant there lets it sleep all the
 * same: it stays in its operation, which is all the table can tell of a
 * participant that waits for its CPU.
 */
static inline void lsv_ht_test_op_wait(struct completion *done)
{
	if (LSV_HT_OPS_NONPREEMPTIBLE)
		preempt_enable();
	wait_for_completion(done);
	if (LSV_HT_OPS_NONPREEMPTIBLE)
		preempt_disable();
}

static inline void lsv_ht_test_op_msleep(unsigned int ms)
{
	if (LSV_HT_OPS_NONPREEMPTIBLE)
		preempt_enable();
	msleep(ms);
	if (LSV_HT_OPS_NONPREEMPTIBLE)
		preempt_disable();
}

typedef void (*lsv_ht_test_job_fn_t)(struct lsv_ht_test_job *job);
#if LSV_HT_HAS_RECOVERY
typedef bool (*lsv_ht_test_until_fn_t)(struct lsv_ht_test_ctx *ctx);
#endif

struct lsv_ht_test_value {
	refcount_t ref;
	/*
	 * Cells of the index that point at this value. A table that recycles a
	 * cell behind the ops that made it, or that gives a cell a value of its
	 * own accord, cannot keep this count straight -- and a consumer that
	 * ties anything to the last cell of a value depends on it.
	 */
	atomic_t cell_cnt;
	u64 key;
	struct list_head link;
	/* in the chain of its key while it may still be live, once published */
	struct list_head key_link;
};

struct lsv_ht_test_ctx {
	struct kunit *test;
	struct lsv_wq *wq[LSV_HT_TEST_WQ_CNT];
	unsigned int worker_cnt;	/* per wq */
	struct lsv_ht *ht;

	atomic_t nodes_live;
	atomic_t nodes_freed;
	/* ops calls about a sentinel, which are the table's own: always 0 */
	atomic_t sentinel_calls;
	int nodes_max;
#if LSV_HT_HAS_RECOVERY
	/* cells node_copy() may take on top of the pool, as a real consumer keeps */
	int recovery_max;
	atomic_t copy_blocked;
	/* when nonzero, node_copy() is refused only for keys up to this one */
	u64 copy_block_max;
	/*
	 * node_copy() falls back on memory outside the pool once the pool and
	 * its reserve are gone, as a real consumer must: see
	 * LSV_DTA_SPARE_CELLS. Cells taken that way are counted apart.
	 */
	bool copy_fallback;
	atomic_t fallback_live;
	atomic_t fallback_cnt;
#endif

	spinlock_t values_lock;	/* protects values, live_by_key */
	struct list_head values;
	struct list_head live_by_key[1 << LSV_HT_TEST_KEY_BITS];
	/*
	 * Values that lost their last cell while still referenced: whatever a
	 * consumer ties to the last cell of a value -- lsv_de_table releases
	 * the entry there -- would have gone from under its holders. Counted
	 * until the table itself is freed, which drops the cells of every
	 * entry still in it.
	 */
	atomic_t released_held;
	bool tearing_down;
	/* the table runs on g_lsv_ht_test_ops_norefs: values are not refcounted */
	bool norefs;

	/* stalled-participant scenario */
	atomic_t park_armed;
	atomic_t park_in_alloc;
	struct completion parked;
	struct completion release;
	u64 park_key;

	/*
	 * The cell a parked traversal stands on, and whether the table has given
	 * it back while the traversal still was to read it.
	 */
	struct lsv_lf_list_node *held_cell;
	bool held_cell_freed;

	/*
	 * Cells the table gave back are kept, marked, until the test is over:
	 * no walk may reach one again, nor any op be asked about one, and the
	 * memory staying valid lets the test count it instead of crashing on it.
	 */
	spinlock_t quarantine_lock;	/* protects quarantine, reusable */
	struct list_head quarantine;
	atomic_t touched_freed;
	/*
	 * Cells given back go to |reusable| instead, and node_alloc() (and
	 * node_copy()) hand them out again with new contents: a walk that still
	 * holds an old address finds another key, value and next pointer there.
	 */
	bool reuse;
	struct list_head reusable;
	atomic_t reused;

	/* preemptible() as seen inside an operation, and by node_alloc(); -1 unseen */
	int op_preemptible;
	int alloc_preemptible;

	/* an insertion parked right before it links its cell */
	atomic_t link_armed;
	struct completion link_parked;
	struct completion link_release;

	/* a second traversal parked on a cell, see lsv_ht_test_cell_reached_stub() */
	atomic_t park2_armed;
	u64 park2_key;
	struct completion parked2;
	struct completion release2;

	/* jobs queued and not finished, which .exit waits for */
	atomic_t jobs_running;

	/* a job that holds its worker, see ht_test_hold_fn() */
	struct completion hold_parked;
	struct completion hold_release;

#if LSV_HT_HAS_RECOVERY
	/* an operation parked between computing its timestamp and publishing it */
	atomic_t begin_armed;
	unsigned int begin_tid;
	struct completion begin_parked;
	struct completion begin_release;

	/* what a churn waits for, see ht_test_churn_until_fn() */
	lsv_ht_test_until_fn_t churn_until;
	unsigned int watch_tid;

	/* a reclamation parked between the flags and the timestamp of a record */
	atomic_t record_armed;
	unsigned int record_tid;
	struct completion record_parked;
	struct completion record_release;
#endif

	/* concurrent-stall scenario */
	u64 bucket_keys[LSV_HT_TEST_STALL_KEYS];
	atomic_t stalled_now;
	atomic_t stalls_left;
	atomic_long_t last_stall;
};

struct lsv_ht_test_job {
	struct lsv_wq_ctx wq_ctx;
	struct lsv_ht_test_ctx *ctx;
	lsv_ht_test_job_fn_t fn;
	struct completion done;
	u64 key;
	unsigned int iterations;
	void *value;
	long result;
	long wrong;
	bool stub_visible;
#if LSV_HT_HAS_RECOVERY
	unsigned int deadline_ms;
	/* lsv_ht_insert_node(): the caller's cell */
	struct lsv_lf_list_node *node;
#endif
	/* keys of the randomized cases, seeded per job from the seed of the case */
	struct rnd_state rnd;
};

extern const struct lsv_lf_list_ops g_lsv_ht_test_ops_norefs;

struct lsv_lf_list_node *lsv_ht_test_node_alloc(u64 key, void *priv);
void lsv_ht_test_node_dealloc(struct lsv_lf_list_node *node, void *priv);
struct lsv_worker *lsv_ht_test_worker(struct lsv_ht_test_ctx *ctx, unsigned int wq_idx,
				      unsigned int worker);
struct lsv_ht_test_job *lsv_ht_test_job_new(struct kunit *test);
void lsv_ht_test_job_start_on(struct lsv_ht_test_ctx *ctx, struct lsv_ht_test_job *job,
			      struct lsv_worker *worker, lsv_ht_test_job_fn_t fn);
void lsv_ht_test_job_start(struct lsv_ht_test_ctx *ctx, struct lsv_ht_test_job *job,
			   unsigned int wq_idx, unsigned int worker,
			   lsv_ht_test_job_fn_t fn);
bool lsv_ht_test_job_wait(struct lsv_ht_test_job *job);
void lsv_ht_test_run(struct lsv_ht_test_ctx *ctx, struct lsv_ht_test_job *job,
		     lsv_ht_test_job_fn_t fn);
void lsv_ht_test_insert_fn(struct lsv_ht_test_job *job);
void lsv_ht_test_find_fn(struct lsv_ht_test_job *job);
void lsv_ht_test_remove_fn(struct lsv_ht_test_job *job);
void *lsv_ht_test_insert(struct lsv_ht_test_ctx *ctx, u64 key, bool *is_new);
void *lsv_ht_test_find(struct lsv_ht_test_ctx *ctx, u64 key);
bool lsv_ht_test_remove(struct lsv_ht_test_ctx *ctx, u64 key);
bool lsv_ht_test_release_key(struct lsv_ht_test_ctx *ctx, u64 key);
void lsv_ht_test_bucket_keys(struct lsv_ht_test_ctx *ctx, u64 *keys, unsigned int cnt);
void lsv_ht_test_cell_reached_stub(struct lsv_lf_list_node *cell);
void lsv_ht_test_cell_linking_stub(struct lsv_lf_list_node *cell);
void lsv_ht_test_park_lookup(struct lsv_ht_test_ctx *ctx, struct lsv_ht_test_job *job,
			     u64 park_key, u64 key);
struct lsv_ht_test_value *lsv_ht_test_insert_eventually(struct lsv_ht_test_ctx *ctx, u64 key);
unsigned int lsv_ht_test_value_published(struct lsv_ht_test_ctx *ctx,
					 struct lsv_ht_test_value *value);
unsigned int lsv_ht_test_values_held(struct lsv_ht_test_ctx *ctx);
u64 lsv_ht_test_seed_get(struct kunit *test);
int lsv_ht_test_create_table_ops(struct lsv_ht_test_ctx *ctx, u64 size, int cells,
				 const struct lsv_lf_list_ops *ops);
int lsv_ht_test_create_table(struct lsv_ht_test_ctx *ctx, u64 size, int cells);
int lsv_ht_test_create_table_reserve(struct lsv_ht_test_ctx *ctx, u64 size, int cells,
				     int reserve);
bool lsv_ht_test_join(struct lsv_ht_test_ctx *ctx);
int lsv_ht_test_init(struct kunit *test);
void lsv_ht_test_exit(struct kunit *test);

/*
 * Provided by the implementation, in its tests/ht_impl_test.c: parks |job| on
 * worker 1 of wq 0 in the middle of an operation on |key|, where it holds back
 * every cell retired from then on, and lets it go again. Whatever the parked
 * operation needs from the pool it has in hand before it parks.
 */
void lsv_ht_test_stall_participant(struct lsv_ht_test_ctx *ctx, struct lsv_ht_test_job *job,
				   u64 key);
void lsv_ht_test_resume_participant(struct lsv_ht_test_ctx *ctx, struct lsv_ht_test_job *job);

#endif /* _LSV_KUNIT_HT_H */
