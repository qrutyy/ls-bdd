#include <kunit/static_stub.h>
#include <kunit/test.h>
#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/math64.h>
#include <linux/prandom.h>
#include <linux/random.h>
#include <linux/sched/clock.h>
#include <linux/slab.h>
#include <linux/timekeeping.h>
#include <kunit/test.h>

#include "utils/kunit.h"
#include "linux/hash.h"
#include "utils/wq.h"
#include "utils/lock-free/ht-dta/ht_dta.h"

#include "../lsv_kunit_ht.h"
#include "../include_static/__static_kunit_lsv_ht.h"

/*
 * Benchmarks of the concurrent index over its whole lifecycle, not a
 * regression test: every case reports its numbers with kunit_info() and
 * fails only when the scenario could not be run.
 *
 * Every participant is an lsv_wq worker bound to its CPU, runs one scenario
 * flat out for a fixed time and times each operation with local_clock(), so
 * the latencies include the few tens of nanoseconds of the clock itself.
 * ns/op is the CPU time of one participant per operation: workers only spin.
 */

#define LSV_HTB_RUN_MS		1000
#define LSV_HTB_KEYS		65536	/* keys of the prefilled cases */
#define LSV_HTB_TABLE		(2 * LSV_HTB_KEYS)
#define LSV_HTB_HOT_KEYS	16	/* one bucket of them */
#define LSV_HTB_CHECK_EVERY	64	/* ops between deadline checks */
#define LSV_HTB_STOP_MS		30000	/* for the jobs to end, see htb_stop() */

/* stalled-participant cases */
#define LSV_HTB_STALL_RUN_MS	6000
#define LSV_HTB_STALL_AFTER_MS	500
#define LSV_HTB_STALL_MS	3000
#define LSV_HTB_POOL_SLACK	4096	/* cells above the entries, bounded pool */

/* log-linear latency histogram: 16 sub-buckets per power of two, ~6% error */
#define LSV_HTB_HIST_SUB_BITS	4
#define LSV_HTB_HIST_SIZE	(64 << LSV_HTB_HIST_SUB_BITS)

struct lsv_htb_value {
	refcount_t ref;
	/* the value goes with its last cell, as lsv_de_table's entries do */
	atomic_t cells;
};

struct lsv_htb_cell {
	struct lsv_lf_list_node node;
};

struct lsv_htb_ctx;

enum lsv_htb_op {
	LSV_HTB_OP_HIT,		/* pin a prefilled key and put the pin back */
	LSV_HTB_OP_MISS,	/* look up a key that is not there */
	LSV_HTB_OP_CHURN,	/* insert a new key, put its only pin, remove it */
	LSV_HTB_OP_HOT,		/* LSV_HTB_OP_HIT on the keys of a single bucket */
	LSV_HTB_OP_CNT,
};

struct lsv_htb_job {
	struct lsv_wq_ctx wq_ctx;
	struct lsv_htb_ctx *ctx;
	struct completion done;
	struct rnd_state rnd;
	unsigned int idx;
	bool stall;		/* the participant the stub stalls */

	u64 ops;
	u64 failed;
	u64 max_ns;
	u32 hist[LSV_HTB_HIST_SIZE];
};

struct lsv_htb_ctx {
	struct kunit *test;
	struct lsv_wq *wq;
	unsigned int worker_cnt;
	struct kmem_cache *cells;

	struct lsv_ht *ht;
	long max_cells;		/* LONG_MAX: unbounded */
	atomic_long_t cells_live;
	atomic_long_t cells_fallback;

	enum lsv_htb_op op;
	u64 hot_keys[LSV_HTB_HOT_KEYS];
	u64 end_ns;

	atomic_t ready;
	struct completion all_ready;
	struct completion go;
	/* jobs queued and not over, which htb_stop() waits for */
	atomic_t jobs_running;

	/* stalled-participant cases */
	atomic_t stall_armed;
};

static struct lsv_lf_list_node *htb_cell_new(struct lsv_htb_ctx *ctx, bool for_copy)
{
	struct lsv_htb_cell *cell;

	if (atomic_long_inc_return(&ctx->cells_live) > ctx->max_cells) {
		/* node_copy() falls back beyond the pool, as the contract asks */
		if (!for_copy) {
			atomic_long_dec(&ctx->cells_live);
			return NULL;
		}
		atomic_long_inc(&ctx->cells_fallback);
	}

	cell = kmem_cache_zalloc(ctx->cells, GFP_KERNEL);
	if (!cell) {
		atomic_long_dec(&ctx->cells_live);
		return NULL;
	}
	return &cell->node;
}

static struct lsv_lf_list_node *htb_node_alloc(u64 key, void *priv)
{
	struct lsv_htb_ctx *ctx = priv;
	struct lsv_lf_list_node *node;
	struct lsv_htb_value *value;

	node = htb_cell_new(ctx, false);
	if (!node)
		return NULL;

	value = kmalloc(sizeof(*value), GFP_KERNEL);
	if (!value) {
		kmem_cache_free(ctx->cells, node);
		atomic_long_dec(&ctx->cells_live);
		return NULL;
	}
	refcount_set(&value->ref, 1);
	atomic_set(&value->cells, 1);

	node->key = key;
	node->value = value;
	return node;
}

static struct lsv_lf_list_node *htb_node_copy(struct lsv_lf_list_node *src, void *priv)
{
	struct lsv_htb_ctx *ctx = priv;
	struct lsv_lf_list_node *node;

	node = htb_cell_new(ctx, true);
	if (!node)
		return NULL;

	node->key = src->key;
	node->value = src->value;
	atomic_inc(&((struct lsv_htb_value *)node->value)->cells);
	return node;
}

static void htb_node_dealloc(struct lsv_lf_list_node *node, void *priv)
{
	struct lsv_htb_value *value = node->value;
	struct lsv_htb_ctx *ctx = priv;

	if (value && atomic_dec_and_test(&value->cells))
		kfree(value);

	kmem_cache_free(ctx->cells, node);
	atomic_long_dec(&ctx->cells_live);
}

static bool htb_value_get(struct lsv_lf_list_node *node, void *priv)
{
	return refcount_inc_not_zero(&((struct lsv_htb_value *)node->value)->ref);
}

static bool htb_value_is_dead(struct lsv_lf_list_node *node, void *priv)
{
	return !refcount_read(&((struct lsv_htb_value *)node->value)->ref);
}

static void htb_value_put_new(struct lsv_lf_list_node *node, void *priv)
{
	WARN_ON(!refcount_dec_and_test(&((struct lsv_htb_value *)node->value)->ref));
}

static const struct lsv_lf_list_ops g_lsv_htb_ops = {
	.node_alloc	= htb_node_alloc,
	.node_copy	= htb_node_copy,
	.node_dealloc	= htb_node_dealloc,
	.value_get	= htb_value_get,
	.value_is_dead	= htb_value_is_dead,
	.value_put_new	= htb_value_put_new,
};

static unsigned int htb_hist_idx(u64 ns)
{
	unsigned int msb;

	if (ns < (1 << LSV_HTB_HIST_SUB_BITS))
		return ns;

	msb = fls64(ns) - 1;
	return ((msb - LSV_HTB_HIST_SUB_BITS + 1) << LSV_HTB_HIST_SUB_BITS) |
	       ((ns >> (msb - LSV_HTB_HIST_SUB_BITS)) & ((1 << LSV_HTB_HIST_SUB_BITS) - 1));
}

/* The lower bound of a histogram bucket. */
static u64 htb_hist_ns(unsigned int idx)
{
	unsigned int msb;

	if (idx < (1 << LSV_HTB_HIST_SUB_BITS))
		return idx;

	msb = (idx >> LSV_HTB_HIST_SUB_BITS) + LSV_HTB_HIST_SUB_BITS - 1;
	return (u64)((1 << LSV_HTB_HIST_SUB_BITS) | (idx & ((1 << LSV_HTB_HIST_SUB_BITS) - 1)))
		<< (msb - LSV_HTB_HIST_SUB_BITS);
}

static u64 htb_percentile(const u32 *hist, u64 total, unsigned int permille10)
{
	u64 want = div_u64(total * permille10, 10000);
	u64 seen = 0;
	unsigned int i;

	for (i = 0; i < LSV_HTB_HIST_SIZE; i++) {
		seen += hist[i];
		if (seen > want)
			return htb_hist_ns(i);
	}
	return 0;
}

/* Keys of the prefilled cases are 1..LSV_HTB_KEYS; churned ones lie far above. */
static u64 htb_churn_key(struct lsv_htb_job *job, u64 n)
{
	return (1ULL << 40) + ((u64)job->idx << 32) + n;
}

static void htb_put(struct lsv_htb_ctx *ctx, struct lsv_htb_value *value, u64 key)
{
	if (refcount_dec_and_test(&value->ref))
		lsv_ht_remove(ctx->ht, key);
}

static bool htb_op_run(struct lsv_htb_job *job, u64 n)
{
	struct lsv_htb_ctx *ctx = job->ctx;
	struct lsv_htb_value *value;
	bool is_new;
	u64 key;

	switch (ctx->op) {
	case LSV_HTB_OP_HIT:
		key = 1 + prandom_u32_state(&job->rnd) % LSV_HTB_KEYS;
		break;
	case LSV_HTB_OP_HOT:
		key = ctx->hot_keys[prandom_u32_state(&job->rnd) % LSV_HTB_HOT_KEYS];
		break;
	case LSV_HTB_OP_MISS:
		key = LSV_HTB_KEYS + 1 + prandom_u32_state(&job->rnd) % LSV_HTB_KEYS;
		return !lsv_ht_find_value(ctx->ht, key);
	case LSV_HTB_OP_CHURN:
		key = htb_churn_key(job, n);
		value = lsv_ht_insert_or_inc_ref(ctx->ht, key, &is_new);
		if (!value)
			return false;
		htb_put(ctx, value, key);
		return is_new;
	default:
		return false;
	}

	value = lsv_ht_insert_or_inc_ref(ctx->ht, key, &is_new);
	if (!value)
		return false;
	htb_put(ctx, value, key);
	return !is_new;
}

/* The stalled participant: one lookup in the hot bucket, stalled on an anchor. */
static void htb_stall_stub(struct lsv_lf_list_node *anchor)
{
	struct kunit *test = kunit_get_current_test();
	struct lsv_htb_ctx *ctx;

	if (!test)
		return;
	ctx = test->priv;
	if (atomic_xchg(&ctx->stall_armed, 0))
		msleep(LSV_HTB_STALL_MS);
}

static void htb_job_run(struct lsv_wq_ctx *wq_ctx)
{
	struct lsv_htb_job *job = container_of(wq_ctx, struct lsv_htb_job, wq_ctx);
	struct lsv_htb_ctx *ctx = job->ctx;
	u64 n = 0;
	u64 t0;
	u64 t1;

	if (atomic_inc_return(&ctx->ready) == ctx->worker_cnt)
		complete(&ctx->all_ready);
	wait_for_completion(&ctx->go);

	if (job->stall) {
		msleep(LSV_HTB_STALL_AFTER_MS);
		current->kunit_test = ctx->test;
		atomic_set(&ctx->stall_armed, 1);
		lsv_ht_find_value(ctx->ht, ctx->hot_keys[LSV_HTB_HOT_KEYS - 1]);
		current->kunit_test = NULL;
		goto out;
	}

	for (;;) {
		if (!(n % LSV_HTB_CHECK_EVERY)) {
			if (ktime_get_ns() >= READ_ONCE(ctx->end_ns))
				break;
			cond_resched();
		}

		t0 = local_clock();
		if (!htb_op_run(job, n))
			job->failed++;
		t1 = local_clock();

		job->hist[htb_hist_idx(t1 - t0)]++;
		if (t1 - t0 > job->max_ns)
			job->max_ns = t1 - t0;
		n++;
	}

	job->ops = n;
out:
	atomic_dec(&ctx->jobs_running);
	complete(&job->done);
}

static int htb_table_create(struct lsv_htb_ctx *ctx, u64 size, long max_cells)
{
	ctx->max_cells = max_cells;
	atomic_long_set(&ctx->cells_live, 0);
	atomic_long_set(&ctx->cells_fallback, 0);
	ctx->ht = lsv_ht_alloc_and_init(size, ctx, &g_lsv_htb_ops);
	if (IS_ERR(ctx->ht)) {
		int rc = PTR_ERR(ctx->ht);

		ctx->ht = NULL;
		return rc;
	}
	return 0;
}

struct lsv_htb_fill {
	struct lsv_wq_ctx wq_ctx;
	struct lsv_htb_ctx *ctx;
	struct completion done;
	u64 failed;
};

/* Every prefilled key holds the pin of its insertion for the whole run. */
static void htb_fill_run(struct lsv_wq_ctx *wq_ctx)
{
	struct lsv_htb_fill *fill = container_of(wq_ctx, struct lsv_htb_fill, wq_ctx);
	struct lsv_htb_ctx *ctx = fill->ctx;
	unsigned int n = 0;
	unsigned int bucket;
	bool is_new;
	u64 key;

	for (key = 1; key <= LSV_HTB_KEYS; key++) {
		if (!lsv_ht_insert_or_inc_ref(ctx->ht, key, &is_new) || !is_new)
			fill->failed++;
		if (!(key % 1024))
			cond_resched();
	}

	/* the hot bucket is filled up to LSV_HTB_HOT_KEYS, beyond the keys above */
	bucket = hash_64(1, ctx->ht->bucket_bits);
	for (key = 1; n < LSV_HTB_HOT_KEYS; key++) {
		if (hash_64(key, ctx->ht->bucket_bits) != bucket)
			continue;
		ctx->hot_keys[n++] = key;
		if (key > LSV_HTB_KEYS && !lsv_ht_insert_or_inc_ref(ctx->ht, key, &is_new))
			fill->failed++;
	}
	atomic_dec(&ctx->jobs_running);
	complete(&fill->done);
}

static void htb_fill(struct lsv_htb_ctx *ctx)
{
	struct lsv_htb_fill *fill = kunit_kzalloc(ctx->test, sizeof(*fill), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(ctx->test, fill);
	fill->ctx = ctx;
	init_completion(&fill->done);
	lsv_wq_ctx_init(&fill->wq_ctx, htb_fill_run);
	atomic_inc(&ctx->jobs_running);
	lsv_worker_add(lsv_wq_map(ctx->wq, NULL, cpumask_nth(0, cpu_online_mask)),
		       &fill->wq_ctx);
	KUNIT_ASSERT_TRUE(ctx->test, wait_for_completion_timeout(&fill->done, 60 * HZ));
	KUNIT_ASSERT_EQ(ctx->test, fill->failed, 0ULL);
}

struct lsv_htb_result {
	u64 ops, failed, max_ns, p50, p99, p999;
	u64 cpu_ns_op;
	long cells_peak;
	u64 recoveries;
};

/* Gets |threads| jobs ready to start at once, none of them queued yet. */
static void htb_start_prepare(struct lsv_htb_ctx *ctx, unsigned int threads)
{
	atomic_set(&ctx->ready, 0);
	ctx->worker_cnt = threads;
	reinit_completion(&ctx->all_ready);
	reinit_completion(&ctx->go);
}

/*
 * Queues job |i| on the worker of the |i|-th online CPU, where it waits for
 * ctx->go. A job is never on the stack of the test: an assertion that aborts
 * the test leaves it queued, and KUnit frees it only after .exit.
 */
static struct lsv_htb_job *htb_job_queue(struct lsv_htb_ctx *ctx, unsigned int i, bool stall,
					 u64 seed)
{
	struct lsv_htb_job *job = kunit_kzalloc(ctx->test, sizeof(*job), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(ctx->test, job);
	job->ctx = ctx;
	job->idx = i;
	job->stall = stall;
	prandom_seed_state(&job->rnd, seed + i);
	init_completion(&job->done);
	lsv_wq_ctx_init(&job->wq_ctx, htb_job_run);
	atomic_inc(&ctx->jobs_running);
	lsv_worker_add(lsv_wq_map(ctx->wq, NULL, cpumask_nth(i, cpu_online_mask)), &job->wq_ctx);
	return job;
}

/*
 * Runs ctx->op on |threads| workers for |run_ms|; |stall| makes the last of
 * them the stalled participant instead.
 */
static void htb_run(struct lsv_htb_ctx *ctx, unsigned int threads, unsigned int run_ms, bool stall,
		    struct lsv_htb_result *res)
{
	unsigned int busy = threads - stall;
	struct kunit *test = ctx->test;
	u64 seed = get_random_u64();
	struct lsv_htb_job **jobs;
	unsigned long deadline;
	unsigned int i;
	u64 total_ns;
	long live;
	u32 *hist;

	jobs = kunit_kcalloc(test, threads, sizeof(*jobs), GFP_KERNEL);
	hist = kunit_kcalloc(test, LSV_HTB_HIST_SIZE, sizeof(*hist), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, jobs);
	KUNIT_ASSERT_NOT_NULL(test, hist);

	htb_start_prepare(ctx, threads);
	for (i = 0; i < threads; i++)
		jobs[i] = htb_job_queue(ctx, i, stall && i == threads - 1, seed);

	KUNIT_ASSERT_TRUE(test, wait_for_completion_timeout(&ctx->all_ready, 10 * HZ));
	res->recoveries = lsv_ht_recovery_cnt(ctx->ht);
	res->cells_peak = atomic_long_read(&ctx->cells_live);
	WRITE_ONCE(ctx->end_ns, ktime_get_ns() + (u64)run_ms * NSEC_PER_MSEC);
	complete_all(&ctx->go);

	/* sample what the table holds while it runs */
	deadline = jiffies + msecs_to_jiffies(run_ms + LSV_HTB_STALL_MS + 10000);
	for (i = 0; i < threads; i++) {
		while (!try_wait_for_completion(&jobs[i]->done)) {
			live = atomic_long_read(&ctx->cells_live);
			if (live > res->cells_peak)
				res->cells_peak = live;
			KUNIT_ASSERT_TRUE(test, time_before(jiffies, deadline));
			usleep_range(10000, 11000);
		}
	}

	memset(res, 0, offsetof(struct lsv_htb_result, cells_peak));
	for (i = 0; i < threads; i++) {
		unsigned int b;

		res->ops += jobs[i]->ops;
		res->failed += jobs[i]->failed;
		res->max_ns = max(res->max_ns, jobs[i]->max_ns);
		for (b = 0; b < LSV_HTB_HIST_SIZE; b++)
			hist[b] += jobs[i]->hist[b];
	}
	res->recoveries = lsv_ht_recovery_cnt(ctx->ht) - res->recoveries;
	res->p50 = htb_percentile(hist, res->ops, 5000);
	res->p99 = htb_percentile(hist, res->ops, 9900);
	res->p999 = htb_percentile(hist, res->ops, 9990);
	total_ns = (u64)run_ms * NSEC_PER_MSEC * busy;
	res->cpu_ns_op = res->ops ? div64_u64(total_ns, res->ops) : 0;
}

/*
 * Ends whatever a case left running, a failed assertion included, and frees
 * its table. Jobs still waiting for the start are let go with no time left to
 * run, and the table goes only once every job is over and while the workers
 * still run the reclaim requests lsv_ht_free() waits for.
 *
 * Every job ends on its own once it is let go, so a job that does not is
 * stuck in the table. That is reported, and then waited for all the same:
 * the job and this fixture are memory of the test, which the job would go on
 * using. Only the timeout of the test runner ends such a hang.
 */
static void htb_stop(struct lsv_htb_ctx *ctx)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(LSV_HTB_STOP_MS);

	WRITE_ONCE(ctx->end_ns, 0);
	complete_all(&ctx->go);

	while (atomic_read(&ctx->jobs_running) && time_before(jiffies, deadline))
		usleep_range(1000, 2000);
	KUNIT_EXPECT_EQ_MSG(ctx->test, atomic_read(&ctx->jobs_running), 0,
			    "a job is stuck in the table: waiting for it without a bound");
	while (atomic_read(&ctx->jobs_running))
		usleep_range(10000, 11000);

	lsv_ht_free(ctx->ht);
	ctx->ht = NULL;
}

static void htb_report(struct kunit *test, const char *name, unsigned int threads,
		       unsigned int run_ms, const struct lsv_htb_result *res)
{
	u64 kops = div_u64(res->ops * 1000, run_ms) / 1000;

	kunit_info(test,
		   "HTB %-10s threads=%u kops/s=%llu ns/op=%llu p50=%llu p99=%llu p99.9=%llu max=%llu failed=%llu recoveries=%llu cells_peak=%ld\n",
		   name, threads, kops, res->cpu_ns_op, res->p50, res->p99, res->p999,
		   res->max_ns, res->failed, res->recoveries, res->cells_peak);
}

static void htb_scaling(struct kunit *test, const char *name, enum lsv_htb_op op, bool prefill)
{
	struct lsv_htb_ctx *ctx = test->priv;
	struct lsv_htb_result res;
	unsigned int threads;

	for (threads = 1; threads <= num_online_cpus(); threads *= 2) {
		KUNIT_ASSERT_EQ(test, htb_table_create(ctx, LSV_HTB_TABLE, LONG_MAX), 0);
		if (prefill)
			htb_fill(ctx);
		ctx->op = op;

		memset(&res, 0, sizeof(res));
		htb_run(ctx, threads, LSV_HTB_RUN_MS, false, &res);
		htb_report(test, name, threads, LSV_HTB_RUN_MS, &res);

		htb_stop(ctx);
	}
}

static void LSV_KTEST(htb_hit)(struct kunit *test)
{
	htb_scaling(test, "hit", LSV_HTB_OP_HIT, true);
}

static void LSV_KTEST(htb_miss)(struct kunit *test)
{
	htb_scaling(test, "miss", LSV_HTB_OP_MISS, true);
}

static void LSV_KTEST(htb_churn)(struct kunit *test)
{
	htb_scaling(test, "churn", LSV_HTB_OP_CHURN, true);
}

static void LSV_KTEST(htb_hot_bucket)(struct kunit *test)
{
	htb_scaling(test, "hot", LSV_HTB_OP_HOT, true);
}

/*
 * One participant stalls inside a traversal for longer than the table waits
 * before it suspects it, while every other one churns. Reclamation is held
 * until the recovery, so the cells retired meanwhile pile up.
 */
static void htb_stalled(struct kunit *test, const char *name, long max_cells)
{
	unsigned int threads = num_online_cpus();
	struct lsv_htb_ctx *ctx = test->priv;
	struct lsv_htb_result res;

	KUNIT_ASSERT_GE(test, threads, 2U);
	KUNIT_ASSERT_EQ(test, htb_table_create(ctx, LSV_HTB_TABLE, max_cells), 0);
	htb_fill(ctx);
	ctx->op = LSV_HTB_OP_CHURN;

	kunit_activate_static_stub(test, ht_anchor_dropped, htb_stall_stub);
	memset(&res, 0, sizeof(res));
	htb_run(ctx, threads, LSV_HTB_STALL_RUN_MS, true, &res);
	kunit_deactivate_static_stub(test, ht_anchor_dropped);

	htb_report(test, name, threads - 1, LSV_HTB_STALL_RUN_MS, &res);
	kunit_info(test, "HTB %-10s cells_from_fallback=%ld table_bytes=%zu\n", name,
		   atomic_long_read(&ctx->cells_fallback),
		   lsv_ht_mem_usage(ctx->ht, sizeof(struct lsv_htb_cell)));
	KUNIT_EXPECT_GT(test, res.recoveries, 0ULL);

	htb_stop(ctx);
}

static void LSV_KTEST(htb_stalled_unbounded)(struct kunit *test)
{
	htb_stalled(test, "stall", LONG_MAX);
}

static void LSV_KTEST(htb_stalled_full_pool)(struct kunit *test)
{
	htb_stalled(test, "stall-pool", LSV_HTB_KEYS + LSV_HTB_POOL_SLACK);
}

/* The memory of a table of LSV_HTB_KEYS entries, sizes of this fixture's cells. */
static void LSV_KTEST(htb_memory)(struct kunit *test)
{
	struct lsv_htb_ctx *ctx = test->priv;
	size_t table;

	KUNIT_ASSERT_EQ(test, htb_table_create(ctx, LSV_HTB_TABLE, LONG_MAX), 0);
	htb_fill(ctx);

	table = lsv_ht_mem_usage(ctx->ht, sizeof(struct lsv_htb_cell));
	kunit_info(test,
		   "HTB memory participants=%u entries=%u buckets=%llu table_bytes=%zu cells=%ld cell_bytes=%zu per_entry=%zu\n",
		   lsv_worker_id_limit(), LSV_HTB_KEYS, ctx->ht->bucket_cnt, table,
		   atomic_long_read(&ctx->cells_live),
		   sizeof(struct lsv_htb_cell),
		   (table + atomic_long_read(&ctx->cells_live) * sizeof(struct lsv_htb_cell)) /
		   LSV_HTB_KEYS);

	htb_stop(ctx);
}

/*
 * The teardown of a case that failed an assertion after queuing its first
 * job, with the job waiting for a start that never comes.
 */
static void LSV_KTEST(htb_stop_before_go)(struct kunit *test)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(LSV_HTB_STOP_MS);
	struct lsv_htb_ctx *ctx = test->priv;

	KUNIT_ASSERT_GE(test, num_online_cpus(), 2U);
	KUNIT_ASSERT_EQ(test, htb_table_create(ctx, LSV_HTB_TABLE, LONG_MAX), 0);
	ctx->op = LSV_HTB_OP_CHURN;

	/* Arrange: the first of two jobs waits for the start */
	htb_start_prepare(ctx, 2);
	htb_job_queue(ctx, 0, false, 0);
	while (!atomic_read(&ctx->ready) && time_before(jiffies, deadline))
		usleep_range(1000, 2000);
	KUNIT_ASSERT_EQ(test, atomic_read(&ctx->ready), 1);

	/* Act */
	htb_stop(ctx);

	/* Assert: the job is over, and the table and its cells are gone */
	KUNIT_EXPECT_EQ(test, atomic_read(&ctx->jobs_running), 0);
	KUNIT_EXPECT_NULL(test, ctx->ht);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&ctx->cells_live), 0L);
}

static int htb_init(struct kunit *test)
{
	struct lsv_htb_ctx *ctx;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	ctx->test = test;
	/* .exit lets every job go, whether the case got to queue any or not */
	init_completion(&ctx->all_ready);
	init_completion(&ctx->go);

	ctx->cells = kmem_cache_create("lsv_htb_cell", sizeof(struct lsv_htb_cell), 0, 0, NULL);
	if (!ctx->cells)
		return -ENOMEM;

	/* one worker per online CPU, created before any table */
	ctx->wq = lsv_wq_create(0, cpu_online_mask, "kunit-htb");
	if (IS_ERR(ctx->wq)) {
		int rc = PTR_ERR(ctx->wq);

		ctx->wq = NULL;
		kmem_cache_destroy(ctx->cells);
		return rc;
	}

	test->priv = ctx;
	return 0;
}

static void htb_exit(struct kunit *test)
{
	struct lsv_htb_ctx *ctx = test->priv;

	if (!ctx)
		return;

	htb_stop(ctx);
	lsv_wq_destroy(ctx->wq);
	kmem_cache_destroy(ctx->cells);
}

static struct kunit_case __cases[] = {
	LSV_KUNIT_CASE(htb_memory),
	LSV_KUNIT_CASE(htb_stop_before_go),
	LSV_KUNIT_CASE_SLOW(htb_hit),
	LSV_KUNIT_CASE_SLOW(htb_miss),
	LSV_KUNIT_CASE_SLOW(htb_hot_bucket),
	LSV_KUNIT_CASE_SLOW(htb_churn),
	LSV_KUNIT_CASE_SLOW(htb_stalled_unbounded),
	LSV_KUNIT_CASE_SLOW(htb_stalled_full_pool),
	{}
};

static struct kunit_suite __suite = {
	.name		= "lsv_ht_dta_perf",
	.init		= htb_init,
	.exit		= htb_exit,
	.test_cases	= __cases,
};

kunit_test_suites(&__suite);
