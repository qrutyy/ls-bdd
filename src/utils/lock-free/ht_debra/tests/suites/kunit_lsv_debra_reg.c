// SPDX-License-Identifier: GPL-2.0-only

/*
 * KUnit suite of the EBR hash table (ht_ebr), for both back-ends: ht=ebr
 * (limbo lists per participant) and ht=ebr+ (limbo lists shared by the
 * table, LSV_HT_EBR_GLOBAL). Where the two must behave differently, a case
 * says what each one must do. Table operations run on lsv_wq workers, the
 * participants of the table; the test thread only queues them.
 *
 * The interface itself is covered by the shared suites (ht_tests/), which run
 * on every table.
 *
 * Cells the table gives back are kept, marked freed, for a while: a walk
 * that steps on one is counted instead of crashing. The quarantine is
 * bounded, so that a long case does not run the machine out of memory; past
 * it, a use after free is KASAN's to catch.
 */

#include <kunit/test.h>
#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/cpumask.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/list.h>
#include <linux/prandom.h>
#include <linux/random.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include "utils/kunit.h"
#include "utils/wq.h"
#include "utils/lock-free/ht_debra/ht_debra.h"
#include "utils/lock-free/ht_debra/debra.h"
#include "utils/lock-free/ht_debra/tests/include_static/__static_kunit_lsv_ht.h"

#define DEBRA_TEST_WAIT_MS	30000
#define DEBRA_TEST_KEYS		256
#define DEBRA_TEST_STRESS_MS	2000
#define DEBRA_TEST_STRESS_KEYS	1024
#define DEBRA_TEST_TABLE_SIZE	(64 * 1024)
#define DEBRA_TEST_QUARANTINE_MAX	(64 * 1024)

#define DEBRA_TEST_ADVANCE_FREQ	debra_test_advance_ops(ctx)
#define DEBRA_TEST_SHARED_LIMBO	false

struct debra_test_cell {
	struct lsv_lf_list_node node;
	struct list_head qlink;
	bool freed;
};

struct debra_test_ctx {
	struct kunit *test;
	struct lsv_wq *wq;
	unsigned int worker_cnt;
	struct lsv_ht *ht;

	/* cells handed out by node_alloc() and not given back */
	atomic_t nodes_live;
	atomic_t nodes_freed;
	/* node_alloc() refuses cells beyond this many live ones */
	atomic_t nodes_max;

	spinlock_t quarantine_lock;	/* protects quarantine, quarantine_cnt */
	struct list_head quarantine;
	unsigned int quarantine_cnt;
	atomic_t touched_freed;

	/* a walk parked on the cell of |park_key|, inside its operation */
	atomic_t park_armed;
	u64 park_key;
	struct debra_test_cell *held;
	struct completion parked;
	struct completion release;

	atomic_t jobs_running;
};

struct debra_test_job;
typedef void (*debra_test_job_fn_t)(struct debra_test_job *job);

struct debra_test_job {
	struct lsv_wq_ctx wq_ctx;
	struct debra_test_ctx *ctx;
	debra_test_job_fn_t fn;
	struct completion done;
	u64 key;
	u64 cnt;
	long result;
	long wrong;
	struct rnd_state rnd;
};

static struct debra_test_ctx *g_debra_test_ctx;

static struct debra_test_cell *debra_test_cell_of(struct lsv_lf_list_node *node)
{
	return container_of(node, struct debra_test_cell, node);
}

static struct lsv_lf_list_node *debra_test_node_alloc(u64 key, void *priv)
{
	struct debra_test_ctx *ctx = priv;
	struct debra_test_cell *cell;

	if (atomic_inc_return(&ctx->nodes_live) > atomic_read(&ctx->nodes_max)) {
		atomic_dec(&ctx->nodes_live);
		return NULL;
	}

	/* may sleep, inside the operation: that holds reclamation up, nothing else */
	cell = kzalloc_obj(*cell);
	if (!cell) {
		atomic_dec(&ctx->nodes_live);
		return NULL;
	}

	cell->node.key = key;
	/* the value is the key: never NULL, as no reserved key gets here */
	cell->node.value = (void *)(uintptr_t)key;

	return &cell->node;
}

static void debra_test_node_dealloc(struct lsv_lf_list_node *node, void *priv)
{
	struct debra_test_ctx *ctx = priv;
	struct debra_test_cell *cell = debra_test_cell_of(node);
	struct debra_test_cell *oldest = NULL;

	if (WARN_ON_ONCE(READ_ONCE(cell->freed)))
		return;

	WRITE_ONCE(cell->freed, true);
	spin_lock(&ctx->quarantine_lock);
	list_add_tail(&cell->qlink, &ctx->quarantine);
	if (++ctx->quarantine_cnt > DEBRA_TEST_QUARANTINE_MAX) {
		oldest = list_first_entry(&ctx->quarantine, struct debra_test_cell, qlink);
		list_del(&oldest->qlink);
		ctx->quarantine_cnt--;
	}
	spin_unlock(&ctx->quarantine_lock);
	kfree(oldest);

	atomic_dec(&ctx->nodes_live);
	atomic_inc(&ctx->nodes_freed);
}

static const struct lsv_lf_list_ops g_debra_test_ops = {
	.node_alloc = debra_test_node_alloc,
	.node_dealloc = debra_test_node_dealloc,
};

static void debra_test_cell_check(struct lsv_lf_list_node *node)
{
	struct debra_test_ctx *ctx = READ_ONCE(g_debra_test_ctx);
	struct debra_test_cell *cell = debra_test_cell_of(node);

	if (!ctx)
		return;

	if (READ_ONCE(cell->freed))
		atomic_inc(&ctx->touched_freed);

	if (node->key == ctx->park_key && atomic_cmpxchg(&ctx->park_armed, 1, 0) == 1) {
		ctx->held = cell;
		complete(&ctx->parked);
		wait_for_completion(&ctx->release);
	}
}

static void debra_test_job_run(struct lsv_wq_ctx *wq_ctx)
{
	struct debra_test_job *job = container_of(wq_ctx, struct debra_test_job, wq_ctx);
	struct debra_test_ctx *ctx = job->ctx;

	job->fn(job);
	complete(&job->done);
	atomic_dec(&ctx->jobs_running);
}

static struct lsv_worker *debra_test_worker(struct debra_test_ctx *ctx, unsigned int i)
{
	return lsv_wq_map(ctx->wq, NULL, cpumask_nth(i, cpu_online_mask));
}

static unsigned int debra_test_tid(struct debra_test_ctx *ctx, unsigned int i)
{
	return lsv_worker_id(debra_test_worker(ctx, i));
}

static struct debra_test_job *debra_test_job_start(struct debra_test_ctx *ctx, unsigned int worker,
					       debra_test_job_fn_t fn, u64 key, u64 cnt)
{
	struct debra_test_job *job = kunit_kzalloc(ctx->test, sizeof(*job), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(ctx->test, job);
	job->ctx = ctx;
	job->fn = fn;
	job->key = key;
	job->cnt = cnt;
	prandom_seed_state(&job->rnd, get_random_u64());
	init_completion(&job->done);
	lsv_wq_ctx_init(&job->wq_ctx, debra_test_job_run);
	atomic_inc(&ctx->jobs_running);
	lsv_worker_add(debra_test_worker(ctx, worker), &job->wq_ctx);

	return job;
}

static void debra_test_job_wait(struct debra_test_job *job)
{
	KUNIT_ASSERT_TRUE_MSG(job->ctx->test,
			      wait_for_completion_timeout(&job->done,
							  msecs_to_jiffies(DEBRA_TEST_WAIT_MS)),
			      "a job is stuck in the table");
}

static void debra_test_wait_for(struct debra_test_ctx *ctx, struct completion *c)
{
	KUNIT_ASSERT_TRUE_MSG(ctx->test,
			      wait_for_completion_timeout(c, msecs_to_jiffies(DEBRA_TEST_WAIT_MS)),
			      "a job never got where the case parks it");
}

static long debra_test_run(struct debra_test_ctx *ctx, unsigned int worker,
			 debra_test_job_fn_t fn, u64 key, u64 cnt)
{
	struct debra_test_job *job = debra_test_job_start(ctx, worker, fn, key, cnt);

	debra_test_job_wait(job);
	KUNIT_EXPECT_EQ(ctx->test, job->wrong, 0);

	return job->result;
}

/* Keys key .. key + cnt - 1: the ones that went in. */
static void debra_test_insert_fn(struct debra_test_job *job)
{
	bool is_new;
	void *value;
	u64 key;

	for (key = job->key; key < job->key + job->cnt; key++) {
		value = lsv_ht_insert_or_inc_ref(job->ctx->ht, key, &is_new);
		if (!value)
			continue;
		if (value != (void *)(uintptr_t)key)
			job->wrong++;
		job->result += is_new;
	}
}

/* The ones found, each with its own value. */
static void debra_test_find_fn(struct debra_test_job *job)
{
	void *value;
	u64 key;

	for (key = job->key; key < job->key + job->cnt; key++) {
		value = lsv_ht_find_value(job->ctx->ht, key);
		if (!value)
			continue;
		if (value != (void *)(uintptr_t)key)
			job->wrong++;
		job->result++;
	}
}

/* The ones removed. */
static void debra_test_remove_fn(struct debra_test_job *job)
{
	u64 key;

	for (key = job->key; key < job->key + job->cnt; key++)
		job->result += lsv_ht_remove(job->ctx->ht, key);
}

/* |cnt| lookups of a key that is not there: operations that retire nothing. */
static void debra_test_idle_fn(struct debra_test_job *job)
{
	u64 i;

	for (i = 0; i < job->cnt; i++)
		(void)lsv_ht_find_value(job->ctx->ht, job->key);
}

static void debra_test_stress_fn(struct debra_test_job *job)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(DEBRA_TEST_STRESS_MS);
	struct lsv_ht *ht = job->ctx->ht;
	bool is_new;
	void *value;
	u64 key;

	while (time_before(jiffies, deadline)) {
		key = 1 + prandom_u32_state(&job->rnd) % DEBRA_TEST_STRESS_KEYS;

		switch (prandom_u32_state(&job->rnd) % 3) {
		case 0:
			value = lsv_ht_insert_or_inc_ref(ht, key, &is_new);
			break;
		case 1:
			value = lsv_ht_find_value(ht, key);
			break;
		default:
			value = NULL;
			lsv_ht_remove(ht, key);
			break;
		}

		if (value && value != (void *)(uintptr_t)key)
			job->wrong++;
		job->result++;
	}
}

static void debra_test_create_table(struct debra_test_ctx *ctx, int cells)
{
	struct lsv_ht *ht;

	atomic_set(&ctx->nodes_max, cells);
	ht = lsv_ht_alloc_and_init(DEBRA_TEST_TABLE_SIZE, ctx, &g_debra_test_ops);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(ctx->test, ht);
	ctx->ht = ht;
}

static void debra_test_need_workers(struct debra_test_ctx *ctx, unsigned int cnt)
{
	if (ctx->worker_cnt < cnt)
		kunit_skip(ctx->test, "needs %u online CPUs", cnt);
}

static long debra_test_advance_ops(struct debra_test_ctx *ctx)
{
	unsigned int n = max_t(unsigned int, ctx->ht->debra->nr_threads,
			       LSV_INCR_THRESHOLD);

	return (long)n * LSV_CHECKS_THRESHOLD;
}

/* Inserted keys are found with their values, removed ones are gone. */
static void LSV_KTEST(debra_basic)(struct kunit *test)
{
	struct debra_test_ctx *ctx = test->priv;

	debra_test_create_table(ctx, DEBRA_TEST_KEYS);

	KUNIT_EXPECT_EQ(test, debra_test_run(ctx, 0, debra_test_insert_fn, 1, DEBRA_TEST_KEYS),
			DEBRA_TEST_KEYS);
	/* a second insertion pins what is there and makes nothing */
	KUNIT_EXPECT_EQ(test, debra_test_run(ctx, 0, debra_test_insert_fn, 1, DEBRA_TEST_KEYS), 0);
	KUNIT_EXPECT_EQ(test, atomic_read(&ctx->nodes_live), DEBRA_TEST_KEYS);
	KUNIT_EXPECT_EQ(test, debra_test_run(ctx, 0, debra_test_find_fn, 1, DEBRA_TEST_KEYS),
			DEBRA_TEST_KEYS);
	KUNIT_EXPECT_EQ(test, lsv_ht_entry_cnt(ctx->ht), DEBRA_TEST_KEYS);

	KUNIT_EXPECT_EQ(test, debra_test_run(ctx, 0, debra_test_remove_fn, 1, DEBRA_TEST_KEYS),
			DEBRA_TEST_KEYS);
	KUNIT_EXPECT_EQ(test, debra_test_run(ctx, 0, debra_test_find_fn, 1, DEBRA_TEST_KEYS), 0);
	KUNIT_EXPECT_EQ(test, debra_test_run(ctx, 0, debra_test_remove_fn, 1, DEBRA_TEST_KEYS), 0);
	KUNIT_EXPECT_TRUE(test, lsv_ht_is_empty(ctx->ht));
}

/*
 * With nobody else in the table, a lone participant moves the epoch on by
 * itself and gets every cell it retired back.
 */
static void LSV_KTEST(debra_reclaim_alone)(struct kunit *test)
{
	struct debra_test_ctx *ctx = test->priv;
	unsigned int tid = debra_test_tid(ctx, 0);
	u64 epoch;

	debra_test_create_table(ctx, DEBRA_TEST_KEYS);
	epoch = lsv_ht_epoch(ctx->ht);

	debra_test_run(ctx, 0, debra_test_insert_fn, 1, DEBRA_TEST_KEYS);
	debra_test_run(ctx, 0, debra_test_remove_fn, 1, DEBRA_TEST_KEYS);
	debra_test_run(ctx, 0, debra_test_idle_fn, DEBRA_TEST_KEYS + 1, 4 * DEBRA_TEST_ADVANCE_FREQ);

	KUNIT_EXPECT_EQ(test, atomic_read(&ctx->nodes_freed), DEBRA_TEST_KEYS);
	KUNIT_EXPECT_EQ(test, atomic_read(&ctx->nodes_live), 0);
	KUNIT_EXPECT_EQ(test, ht_debra_limbo_total(ctx->ht), 0);
	KUNIT_EXPECT_GE(test, lsv_ht_epoch(ctx->ht), epoch + 2);
	KUNIT_EXPECT_FALSE(test, ht_debra_is_active(ctx->ht, tid));
}

/*
 * A participant parked inside an operation holds the epoch: at most one
 * advance gets past it, nothing retired from then on is freed -- the cell it
 * stands on least of all -- and everything is once it moves on.
 */
static void LSV_KTEST(debra_stalled_reader)(struct kunit *test)
{
	struct debra_test_ctx *ctx = test->priv;
	const u64 park_key = DEBRA_TEST_KEYS + 1;
	struct debra_test_job *reader;
	u64 epoch;

	debra_test_need_workers(ctx, 2);
	debra_test_create_table(ctx, 2 * DEBRA_TEST_KEYS);

	debra_test_run(ctx, 1, debra_test_insert_fn, 1, DEBRA_TEST_KEYS + 1);

	ctx->park_key = park_key;
	atomic_set(&ctx->park_armed, 1);
	reader = debra_test_job_start(ctx, 0, debra_test_find_fn, park_key, 1);
	debra_test_wait_for(ctx, &ctx->parked);
	KUNIT_EXPECT_TRUE(test, ht_debra_is_active(ctx->ht, debra_test_tid(ctx, 0)));
	epoch = lsv_ht_epoch(ctx->ht);

	/* the cell under the reader goes too */
	KUNIT_EXPECT_EQ(test, debra_test_run(ctx, 1, debra_test_remove_fn, 1, DEBRA_TEST_KEYS + 1),
			DEBRA_TEST_KEYS + 1);
	debra_test_run(ctx, 1, debra_test_idle_fn, park_key + 1, 4 * DEBRA_TEST_ADVANCE_FREQ);

	KUNIT_EXPECT_EQ(test, atomic_read(&ctx->nodes_freed), 0);
	KUNIT_EXPECT_FALSE(test, READ_ONCE(ctx->held->freed));
	KUNIT_EXPECT_LE(test, lsv_ht_epoch(ctx->ht), epoch + 1);

	complete(&ctx->release);
	debra_test_job_wait(reader);

	/* the reader may have unlinked, and retired, a cell on its way back */
	debra_test_run(ctx, 0, debra_test_idle_fn, park_key + 1, 4 * DEBRA_TEST_ADVANCE_FREQ);
	debra_test_run(ctx, 1, debra_test_idle_fn, park_key + 1, 4 * DEBRA_TEST_ADVANCE_FREQ);
	KUNIT_EXPECT_EQ(test, atomic_read(&ctx->nodes_freed), DEBRA_TEST_KEYS + 1);
	KUNIT_EXPECT_EQ(test, atomic_read(&ctx->nodes_live), 0);
}

/*
 * Where the back-ends part: cells retired by a participant that then goes
 * idle. Another participant moves the epoch on well past them.
 *
 *   ht=ebr   the cells are their retirer's, and wait in its lists: nobody
 *             else may free them, however far the epoch has gone.
 *   ht=ebr+  the advances of the other participant free them.
 */
static void LSV_KTEST(debra_idle_participant_garbage)(struct kunit *test)
{
	struct debra_test_ctx *ctx = test->priv;
	/* few enough operations for the idle participant not to advance itself */
	long cnt;
	u64 epoch;

	debra_test_need_workers(ctx, 2);
	debra_test_create_table(ctx, DEBRA_TEST_KEYS);
	cnt = DEBRA_TEST_ADVANCE_FREQ / 4;

	debra_test_run(ctx, 0, debra_test_insert_fn, 1, cnt);
	KUNIT_ASSERT_EQ(test, debra_test_run(ctx, 0, debra_test_remove_fn, 1, cnt), cnt);
	KUNIT_ASSERT_EQ(test, ht_debra_limbo_total(ctx->ht), cnt);
	epoch = lsv_ht_epoch(ctx->ht);

	debra_test_run(ctx, 1, debra_test_idle_fn, cnt + 1, 4 * DEBRA_TEST_ADVANCE_FREQ);
	KUNIT_EXPECT_GE(test, lsv_ht_epoch(ctx->ht), epoch + 2);

	if (DEBRA_TEST_SHARED_LIMBO) {
		KUNIT_EXPECT_EQ(test, atomic_read(&ctx->nodes_freed), cnt);
		KUNIT_EXPECT_EQ(test, ht_debra_limbo_total(ctx->ht), 0);
	} else {
		KUNIT_EXPECT_EQ(test, atomic_read(&ctx->nodes_freed), 0);
		KUNIT_EXPECT_EQ(test, ht_debra_limbo_total(ctx->ht), cnt);
	}
	KUNIT_EXPECT_FALSE(test, ht_kunit_reclaim_requests_pending(ctx->ht));
}

/* Every worker at once on a few keys: no walk ever reaches a freed cell. */
static void LSV_KTEST(debra_stress)(struct kunit *test)
{
	struct debra_test_ctx *ctx = test->priv;
	struct debra_test_job **jobs;
	unsigned int i;

	debra_test_need_workers(ctx, 2);
	debra_test_create_table(ctx, INT_MAX);

	jobs = kunit_kcalloc(test, ctx->worker_cnt, sizeof(*jobs), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, jobs);

	for (i = 0; i < ctx->worker_cnt; i++)
		jobs[i] = debra_test_job_start(ctx, i, debra_test_stress_fn, 0, 0);
	for (i = 0; i < ctx->worker_cnt; i++) {
		debra_test_job_wait(jobs[i]);
		KUNIT_EXPECT_EQ(test, jobs[i]->wrong, 0);
		KUNIT_EXPECT_GT(test, jobs[i]->result, 0);
	}

	KUNIT_EXPECT_GT(test, atomic_read(&ctx->nodes_freed), 0);
	KUNIT_EXPECT_LE(test, lsv_ht_entry_cnt(ctx->ht), DEBRA_TEST_STRESS_KEYS);
}

static int debra_test_init(struct kunit *test)
{
	struct debra_test_ctx *ctx;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->test = test;
	spin_lock_init(&ctx->quarantine_lock);
	INIT_LIST_HEAD(&ctx->quarantine);
	init_completion(&ctx->parked);
	init_completion(&ctx->release);
	/* no key is the tail sentinel's: nothing parks until a case says so */
	ctx->park_key = LSV_HT_KEY_TAIL_SENTINEL;
	test->priv = ctx;

	/* the workers come first: a table counts only those made before it */
	ctx->wq = lsv_wq_create(0, cpu_online_mask, "kunit-debra");
	if (IS_ERR(ctx->wq)) {
		int rc = PTR_ERR(ctx->wq);

		ctx->wq = NULL;
		return rc;
	}
	ctx->worker_cnt = num_online_cpus();

	WRITE_ONCE(g_debra_test_ctx, ctx);
	WRITE_ONCE(g_lsv_ht_kunit_cell_check, debra_test_cell_check);

	return 0;
}

static void debra_test_exit(struct kunit *test)
{
	struct debra_test_ctx *ctx = test->priv;
	struct debra_test_cell *cell;
	struct debra_test_cell *next;
	unsigned long deadline;

	if (!ctx)
		return;

	/* a case that failed half-way may have left a job parked */
	atomic_set(&ctx->park_armed, 0);
	complete_all(&ctx->release);
	while (atomic_read(&ctx->jobs_running))
		usleep_range(1000, 2000);

	/* the table goes while the workers still run its reclaim requests */
	deadline = jiffies + msecs_to_jiffies(DEBRA_TEST_WAIT_MS);
	while (ctx->ht && ht_kunit_reclaim_requests_pending(ctx->ht) &&
	       time_before(jiffies, deadline))
		usleep_range(1000, 2000);
	lsv_ht_free(ctx->ht);

	if (ctx->wq)
		lsv_wq_destroy(ctx->wq);
	WRITE_ONCE(g_lsv_ht_kunit_cell_check, NULL);
	WRITE_ONCE(g_debra_test_ctx, NULL);

	KUNIT_EXPECT_EQ(test, atomic_read(&ctx->nodes_live), 0);
	KUNIT_EXPECT_EQ(test, atomic_read(&ctx->touched_freed), 0);

	list_for_each_entry_safe(cell, next, &ctx->quarantine, qlink)
		kfree(cell);
}

static struct kunit_case g_lsv_ht_debra_cases[] = {
	LSV_KUNIT_CASE(debra_basic),
	LSV_KUNIT_CASE(debra_reclaim_alone),
	LSV_KUNIT_CASE(debra_stalled_reader),
	LSV_KUNIT_CASE(debra_idle_participant_garbage),
	LSV_KUNIT_CASE_SLOW(debra_stress),
	{}
};

static struct kunit_suite g_lsv_ht_debra_suite = {
	.name = "lsv_ht_debra_local",
	.init = debra_test_init,
	.exit = debra_test_exit,
	.test_cases = g_lsv_ht_debra_cases,
};

kunit_test_suites(&g_lsv_ht_debra_suite);
