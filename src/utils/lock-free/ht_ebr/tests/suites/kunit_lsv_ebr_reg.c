// SPDX-License-Identifier: GPL-2.0-only

/*
 * KUnit suite of the EBR hash table (ht_ebr), for both back-ends: ht=ebr
 * (limbo lists per participant) and ht=ebr+ (limbo lists shared by the
 * table, LSV_HT_EBR_GLOBAL). Where the two must behave differently, a case
 * says what each one must do. Table operations run on lsv_shard_wq workers, the
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
#include "core/wq/wq.h"
#include "utils/lock-free/ht_ebr/ht_ebr.h"
#include "utils/lock-free/ht_ebr/ebr.h"
#include "utils/lock-free/ht_ebr/tests/include_static/__static_kunit_lsv_ht.h"

#define EBR_TEST_WAIT_MS	30000
#define EBR_TEST_KEYS		256
#define EBR_TEST_STRESS_MS	2000
#define EBR_TEST_STRESS_KEYS	1024
#define EBR_TEST_TABLE_SIZE	(64 * 1024)
#define EBR_TEST_QUARANTINE_MAX	(64 * 1024)

#ifdef LSV_HT_EBR_GLOBAL
#define EBR_TEST_SHARED_LIMBO	true
#else
#define EBR_TEST_SHARED_LIMBO	false
#endif

struct ebr_test_cell {
	struct lsv_lf_list_node node;
	struct list_head qlink;
	bool freed;
};

struct ebr_test_ctx {
	struct kunit *test;
	struct lsv_shard_wq *wq;
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
	struct ebr_test_cell *held;
	struct completion parked;
	struct completion release;

	/* an advance parked right after its scan, see ebr_test_scanned() */
	atomic_t scan_armed;
	u64 scan_epoch;
	struct completion scan_parked;
	struct completion scan_release;

	atomic_t jobs_running;
};

struct ebr_test_job;
typedef void (*ebr_test_job_fn_t)(struct ebr_test_job *job);

struct ebr_test_job {
	struct lsv_shard_wq_work wq_ctx;
	struct ebr_test_ctx *ctx;
	ebr_test_job_fn_t fn;
	struct completion done;
	u64 key;
	u64 cnt;
	long result;
	long wrong;
	struct rnd_state rnd;
};

static struct ebr_test_ctx *g_ebr_test_ctx;

static struct ebr_test_cell *ebr_test_cell_of(struct lsv_lf_list_node *node)
{
	return container_of(node, struct ebr_test_cell, node);
}

static struct lsv_lf_list_node *ebr_test_node_alloc(u64 key, void *priv)
{
	struct ebr_test_ctx *ctx = priv;
	struct ebr_test_cell *cell;

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

static void ebr_test_node_dealloc(struct lsv_lf_list_node *node, void *priv)
{
	struct ebr_test_ctx *ctx = priv;
	struct ebr_test_cell *cell = ebr_test_cell_of(node);
	struct ebr_test_cell *oldest = NULL;

	if (WARN_ON_ONCE(READ_ONCE(cell->freed)))
		return;

	WRITE_ONCE(cell->freed, true);
	spin_lock(&ctx->quarantine_lock);
	list_add_tail(&cell->qlink, &ctx->quarantine);
	if (++ctx->quarantine_cnt > EBR_TEST_QUARANTINE_MAX) {
		oldest = list_first_entry(&ctx->quarantine, struct ebr_test_cell, qlink);
		list_del(&oldest->qlink);
		ctx->quarantine_cnt--;
	}
	spin_unlock(&ctx->quarantine_lock);
	kfree(oldest);

	atomic_dec(&ctx->nodes_live);
	atomic_inc(&ctx->nodes_freed);
}

static const struct lsv_lf_list_ops g_ebr_test_ops = {
	.node_alloc = ebr_test_node_alloc,
	.node_dealloc = ebr_test_node_dealloc,
};

static void ebr_test_cell_check(struct lsv_lf_list_node *node)
{
	struct ebr_test_ctx *ctx = READ_ONCE(g_ebr_test_ctx);
	struct ebr_test_cell *cell = ebr_test_cell_of(node);

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

/* g_lsv_ht_ebr_scanned: parks the first advance after the case arms it. */
static void ebr_test_scanned(struct lsv_ht *ht, u64 e)
{
	struct ebr_test_ctx *ctx = READ_ONCE(g_ebr_test_ctx);

	if (!ctx || atomic_cmpxchg(&ctx->scan_armed, 1, 0) != 1)
		return;

	ctx->scan_epoch = e;
	complete(&ctx->scan_parked);
	wait_for_completion(&ctx->scan_release);
}

static void ebr_test_job_run(struct lsv_shard_wq_work *wq_ctx)
{
	struct ebr_test_job *job = container_of(wq_ctx, struct ebr_test_job, wq_ctx);
	struct ebr_test_ctx *ctx = job->ctx;

	job->fn(job);
	complete(&job->done);
	atomic_dec(&ctx->jobs_running);
}

static struct lsv_shard_wq_worker *ebr_test_worker(struct ebr_test_ctx *ctx, unsigned int i)
{
	return lsv_shard_wq_map(ctx->wq, NULL, cpumask_nth(i, cpu_online_mask));
}

static unsigned int ebr_test_tid(struct ebr_test_ctx *ctx, unsigned int i)
{
	return lsv_shard_wq_worker_id(ebr_test_worker(ctx, i));
}

static struct ebr_test_job *ebr_test_job_start(struct ebr_test_ctx *ctx, unsigned int worker,
					       ebr_test_job_fn_t fn, u64 key, u64 cnt)
{
	struct ebr_test_job *job = kunit_kzalloc(ctx->test, sizeof(*job), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(ctx->test, job);
	job->ctx = ctx;
	job->fn = fn;
	job->key = key;
	job->cnt = cnt;
	prandom_seed_state(&job->rnd, get_random_u64());
	init_completion(&job->done);
	lsv_shard_wq_init_work(&job->wq_ctx, ebr_test_job_run);
	atomic_inc(&ctx->jobs_running);
	lsv_shard_wq_queue_work_by_id(lsv_shard_wq_worker_id(ebr_test_worker(ctx, worker)),
				       &job->wq_ctx);

	return job;
}

static void ebr_test_job_wait(struct ebr_test_job *job)
{
	KUNIT_ASSERT_TRUE_MSG(job->ctx->test,
			      wait_for_completion_timeout(&job->done,
							  msecs_to_jiffies(EBR_TEST_WAIT_MS)),
			      "a job is stuck in the table");
}

static void ebr_test_wait_for(struct ebr_test_ctx *ctx, struct completion *c)
{
	KUNIT_ASSERT_TRUE_MSG(ctx->test,
			      wait_for_completion_timeout(c, msecs_to_jiffies(EBR_TEST_WAIT_MS)),
			      "a job never got where the case parks it");
}

static long ebr_test_run(struct ebr_test_ctx *ctx, unsigned int worker,
			 ebr_test_job_fn_t fn, u64 key, u64 cnt)
{
	struct ebr_test_job *job = ebr_test_job_start(ctx, worker, fn, key, cnt);

	ebr_test_job_wait(job);
	KUNIT_EXPECT_EQ(ctx->test, job->wrong, 0);

	return job->result;
}

/* Keys key .. key + cnt - 1: the ones that went in. */
static void ebr_test_insert_fn(struct ebr_test_job *job)
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
static void ebr_test_find_fn(struct ebr_test_job *job)
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
static void ebr_test_remove_fn(struct ebr_test_job *job)
{
	u64 key;

	for (key = job->key; key < job->key + job->cnt; key++)
		job->result += lsv_ht_remove(job->ctx->ht, key);
}

/* |cnt| lookups of a key that is not there: operations that retire nothing. */
static void ebr_test_idle_fn(struct ebr_test_job *job)
{
	u64 i;

	for (i = 0; i < job->cnt; i++)
		(void)lsv_ht_find_value(job->ctx->ht, job->key);
}

static void ebr_test_stress_fn(struct ebr_test_job *job)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(EBR_TEST_STRESS_MS);
	struct lsv_ht *ht = job->ctx->ht;
	bool is_new;
	void *value;
	u64 key;

	while (time_before(jiffies, deadline)) {
		key = 1 + prandom_u32_state(&job->rnd) % EBR_TEST_STRESS_KEYS;

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

static void ebr_test_create_table(struct ebr_test_ctx *ctx, int cells)
{
	struct lsv_ht *ht;

	atomic_set(&ctx->nodes_max, cells);
	ht = lsv_ht_alloc_and_init(EBR_TEST_TABLE_SIZE, ctx, &g_ebr_test_ops);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(ctx->test, ht);
	ctx->ht = ht;
}

static void ebr_test_need_workers(struct ebr_test_ctx *ctx, unsigned int cnt)
{
	if (ctx->worker_cnt < cnt)
		kunit_skip(ctx->test, "needs %u online CPUs", cnt);
}

/* Inserted keys are found with their values, removed ones are gone. */
static void LSV_KTEST(ebr_basic)(struct kunit *test)
{
	struct ebr_test_ctx *ctx = test->priv;

	ebr_test_create_table(ctx, EBR_TEST_KEYS);

	KUNIT_EXPECT_EQ(test, ebr_test_run(ctx, 0, ebr_test_insert_fn, 1, EBR_TEST_KEYS),
			EBR_TEST_KEYS);
	/* a second insertion pins what is there and makes nothing */
	KUNIT_EXPECT_EQ(test, ebr_test_run(ctx, 0, ebr_test_insert_fn, 1, EBR_TEST_KEYS), 0);
	KUNIT_EXPECT_EQ(test, atomic_read(&ctx->nodes_live), EBR_TEST_KEYS);
	KUNIT_EXPECT_EQ(test, ebr_test_run(ctx, 0, ebr_test_find_fn, 1, EBR_TEST_KEYS),
			EBR_TEST_KEYS);
	KUNIT_EXPECT_EQ(test, lsv_ht_entry_cnt(ctx->ht), EBR_TEST_KEYS);

	KUNIT_EXPECT_EQ(test, ebr_test_run(ctx, 0, ebr_test_remove_fn, 1, EBR_TEST_KEYS),
			EBR_TEST_KEYS);
	KUNIT_EXPECT_EQ(test, ebr_test_run(ctx, 0, ebr_test_find_fn, 1, EBR_TEST_KEYS), 0);
	KUNIT_EXPECT_EQ(test, ebr_test_run(ctx, 0, ebr_test_remove_fn, 1, EBR_TEST_KEYS), 0);
	KUNIT_EXPECT_TRUE(test, lsv_ht_is_empty(ctx->ht));
}

/*
 * With nobody else in the table, a lone participant moves the epoch on by
 * itself and gets every cell it retired back.
 */
static void LSV_KTEST(ebr_reclaim_alone)(struct kunit *test)
{
	struct ebr_test_ctx *ctx = test->priv;
	unsigned int tid = ebr_test_tid(ctx, 0);
	u64 epoch;

	ebr_test_create_table(ctx, EBR_TEST_KEYS);
	epoch = lsv_ht_epoch(ctx->ht);

	ebr_test_run(ctx, 0, ebr_test_insert_fn, 1, EBR_TEST_KEYS);
	ebr_test_run(ctx, 0, ebr_test_remove_fn, 1, EBR_TEST_KEYS);
	ebr_test_run(ctx, 0, ebr_test_idle_fn, EBR_TEST_KEYS + 1, 2 * LSV_EBR_ADVANCE_FREQ);

	KUNIT_EXPECT_EQ(test, atomic_read(&ctx->nodes_freed), EBR_TEST_KEYS);
	KUNIT_EXPECT_EQ(test, atomic_read(&ctx->nodes_live), 0);
	KUNIT_EXPECT_EQ(test, ht_ebr_limbo_total(ctx->ht), 0);
	KUNIT_EXPECT_GE(test, lsv_ht_epoch(ctx->ht), epoch + 2);
	KUNIT_EXPECT_FALSE(test, ht_ebr_is_active(ctx->ht, tid));
}

/*
 * A participant parked inside an operation holds the epoch: at most one
 * advance gets past it, nothing retired from then on is freed -- the cell it
 * stands on least of all -- and everything is once it moves on.
 */
static void LSV_KTEST(ebr_stalled_reader)(struct kunit *test)
{
	struct ebr_test_ctx *ctx = test->priv;
	const u64 park_key = EBR_TEST_KEYS + 1;
	struct ebr_test_job *reader;
	u64 epoch;

	ebr_test_need_workers(ctx, 2);
	ebr_test_create_table(ctx, 2 * EBR_TEST_KEYS);

	ebr_test_run(ctx, 1, ebr_test_insert_fn, 1, EBR_TEST_KEYS + 1);

	ctx->park_key = park_key;
	atomic_set(&ctx->park_armed, 1);
	reader = ebr_test_job_start(ctx, 0, ebr_test_find_fn, park_key, 1);
	ebr_test_wait_for(ctx, &ctx->parked);
	KUNIT_EXPECT_TRUE(test, ht_ebr_is_active(ctx->ht, ebr_test_tid(ctx, 0)));
	epoch = lsv_ht_epoch(ctx->ht);

	/* the cell under the reader goes too */
	KUNIT_EXPECT_EQ(test, ebr_test_run(ctx, 1, ebr_test_remove_fn, 1, EBR_TEST_KEYS + 1),
			EBR_TEST_KEYS + 1);
	ebr_test_run(ctx, 1, ebr_test_idle_fn, park_key + 1, 4 * LSV_EBR_ADVANCE_FREQ);

	KUNIT_EXPECT_EQ(test, atomic_read(&ctx->nodes_freed), 0);
	KUNIT_EXPECT_FALSE(test, READ_ONCE(ctx->held->freed));
	KUNIT_EXPECT_LE(test, lsv_ht_epoch(ctx->ht), epoch + 1);

	complete(&ctx->release);
	ebr_test_job_wait(reader);

	/* the reader may have unlinked, and retired, a cell on its way back */
	ebr_test_run(ctx, 0, ebr_test_idle_fn, park_key + 1, 4 * LSV_EBR_ADVANCE_FREQ);
	ebr_test_run(ctx, 1, ebr_test_idle_fn, park_key + 1, 4 * LSV_EBR_ADVANCE_FREQ);
	KUNIT_EXPECT_EQ(test, atomic_read(&ctx->nodes_freed), EBR_TEST_KEYS + 1);
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
static void LSV_KTEST(ebr_idle_participant_garbage)(struct kunit *test)
{
	struct ebr_test_ctx *ctx = test->priv;
	/* few enough operations for the idle participant not to advance itself */
	const long cnt = LSV_EBR_ADVANCE_FREQ / 4;
	u64 epoch;

	ebr_test_need_workers(ctx, 2);
	ebr_test_create_table(ctx, EBR_TEST_KEYS);

	ebr_test_run(ctx, 0, ebr_test_insert_fn, 1, cnt);
	KUNIT_ASSERT_EQ(test, ebr_test_run(ctx, 0, ebr_test_remove_fn, 1, cnt), cnt);
	KUNIT_ASSERT_EQ(test, ht_ebr_limbo_total(ctx->ht), cnt);
	epoch = lsv_ht_epoch(ctx->ht);

	ebr_test_run(ctx, 1, ebr_test_idle_fn, cnt + 1, 4 * LSV_EBR_ADVANCE_FREQ);
	KUNIT_EXPECT_GE(test, lsv_ht_epoch(ctx->ht), epoch + 2);

	if (EBR_TEST_SHARED_LIMBO) {
		KUNIT_EXPECT_EQ(test, atomic_read(&ctx->nodes_freed), cnt);
		KUNIT_EXPECT_EQ(test, ht_ebr_limbo_total(ctx->ht), 0);
	} else {
		KUNIT_EXPECT_EQ(test, atomic_read(&ctx->nodes_freed), 0);
		KUNIT_EXPECT_EQ(test, ht_ebr_limbo_total(ctx->ht), cnt);
	}
	KUNIT_EXPECT_FALSE(test, ht_kunit_reclaim_requests_pending(ctx->ht));
}

/*
 * The whole pool in the limbo of a participant that went idle, and another
 * one that wants a cell.
 *
 *   ht=ebr   the insertion fails: the cells are not its to free. It asks
 *             the idle participant to reclaim, on that one's worker, and the
 *             next insertion gets a cell.
 *   ht=ebr+  the insertion advances the epoch twice, frees the cells
 *             itself, and gets one at once.
 */
static void LSV_KTEST(ebr_out_of_cells)(struct kunit *test)
{
	struct ebr_test_ctx *ctx = test->priv;
	const long cnt = LSV_EBR_ADVANCE_FREQ / 4;
	unsigned long deadline;
	long got;

	ebr_test_need_workers(ctx, 2);
	ebr_test_create_table(ctx, cnt);

	ebr_test_run(ctx, 0, ebr_test_insert_fn, 1, cnt);
	KUNIT_EXPECT_EQ(test, ebr_test_run(ctx, 0, ebr_test_remove_fn, 1, cnt), cnt);
	KUNIT_ASSERT_EQ(test, ht_ebr_limbo_total(ctx->ht), cnt);
	KUNIT_ASSERT_EQ(test, atomic_read(&ctx->nodes_live), cnt);

	got = ebr_test_run(ctx, 1, ebr_test_insert_fn, cnt + 1, 1);

	if (EBR_TEST_SHARED_LIMBO) {
		KUNIT_EXPECT_EQ(test, got, 1);
		KUNIT_EXPECT_EQ(test, atomic_read(&ctx->nodes_freed), cnt);
		KUNIT_EXPECT_FALSE(test, ht_kunit_reclaim_requests_pending(ctx->ht));
	} else {
		KUNIT_EXPECT_EQ(test, got, 0);

		deadline = jiffies + msecs_to_jiffies(EBR_TEST_WAIT_MS);
		while ((ht_kunit_reclaim_requests_pending(ctx->ht) || ht_ebr_limbo_total(ctx->ht)) &&
		       time_before(jiffies, deadline))
			usleep_range(1000, 2000);

		KUNIT_EXPECT_EQ(test, ht_ebr_limbo_total(ctx->ht), 0);
		KUNIT_EXPECT_EQ(test, atomic_read(&ctx->nodes_live), 0);
		KUNIT_EXPECT_EQ(test, ebr_test_run(ctx, 1, ebr_test_insert_fn, cnt + 1, 1), 1);
	}
	KUNIT_EXPECT_EQ(test, atomic_read(&ctx->nodes_live), 1);
}

/*
 * An advancer that scanned epoch E and was held up before it moved anything,
 * while the others moved the epoch on to some X = E + 2 (mod 3) and retired
 * cells in X under a reader that still stands on one of them.
 *
 * In ht=ebr+ the list of X is the one an advance from E detaches, the list of
 * E - 1: a stale advancer that went ahead regardless would free the cell under
 * the reader. The advance is claimed, and the claim checks that the epoch is
 * still E. In ht=ebr the stale compare-and-swap of the epoch fails, and an
 * advance frees nothing anyway.
 *
 * Either way, nothing may be freed while the reader stands where it does:
 * the epoch cannot get past X + 1 meanwhile.
 */
static void LSV_KTEST(ebr_stale_advancer)(struct kunit *test)
{
	struct ebr_test_ctx *ctx = test->priv;
	const u64 park_key = 1;
	struct ebr_test_job *advancer;
	struct ebr_test_job *reader;
	unsigned int rounds;
	u64 stale;
	u64 want;
	u64 e;

	ebr_test_need_workers(ctx, 3);
	ebr_test_create_table(ctx, EBR_TEST_KEYS);
	ebr_test_run(ctx, 1, ebr_test_insert_fn, 1, EBR_TEST_KEYS / 2);

	/* a fresh participant: its first scan comes with its first paced attempt */
	atomic_set(&ctx->scan_armed, 1);
	advancer = ebr_test_job_start(ctx, 0, ebr_test_idle_fn, EBR_TEST_KEYS + 1,
				      LSV_EBR_ADVANCE_FREQ);
	ebr_test_wait_for(ctx, &ctx->scan_parked);
	stale = ctx->scan_epoch;
	want = (stale + 2) % LSV_EBR_NR_EPOCHS;

	for (rounds = 0; rounds < 16; rounds++) {
		e = lsv_ht_epoch(ctx->ht);
		if (e > stale && e % LSV_EBR_NR_EPOCHS == want)
			break;
		ebr_test_run(ctx, 1, ebr_test_idle_fn, EBR_TEST_KEYS + 1, LSV_EBR_ADVANCE_FREQ);
	}
	e = lsv_ht_epoch(ctx->ht);
	KUNIT_ASSERT_GT(test, e, stale);
	KUNIT_ASSERT_EQ(test, e % LSV_EBR_NR_EPOCHS, want);

	/* a reader in X, on the cell of the first key */
	ctx->park_key = park_key;
	atomic_set(&ctx->park_armed, 1);
	reader = ebr_test_job_start(ctx, 2, ebr_test_find_fn, park_key, 1);
	ebr_test_wait_for(ctx, &ctx->parked);
	KUNIT_ASSERT_EQ(test, lsv_ht_epoch(ctx->ht), e);

	/* retired in X: the very first removal, before any attempt to advance */
	KUNIT_EXPECT_EQ(test, ebr_test_run(ctx, 1, ebr_test_remove_fn, 1, EBR_TEST_KEYS / 2),
			EBR_TEST_KEYS / 2);

	complete(&ctx->scan_release);
	ebr_test_job_wait(advancer);

	/* more attempts to advance, none of which may get past the reader */
	ebr_test_run(ctx, 1, ebr_test_idle_fn, EBR_TEST_KEYS + 1, 4 * LSV_EBR_ADVANCE_FREQ);

	KUNIT_EXPECT_LE(test, lsv_ht_epoch(ctx->ht), e + 1);
	KUNIT_EXPECT_FALSE(test, READ_ONCE(ctx->held->freed));
	KUNIT_EXPECT_EQ(test, atomic_read(&ctx->nodes_freed), 0);

	complete(&ctx->release);
	ebr_test_job_wait(reader);
}

/* Every worker at once on a few keys: no walk ever reaches a freed cell. */
static void LSV_KTEST(ebr_stress)(struct kunit *test)
{
	struct ebr_test_ctx *ctx = test->priv;
	struct ebr_test_job **jobs;
	unsigned int i;

	ebr_test_need_workers(ctx, 2);
	ebr_test_create_table(ctx, INT_MAX);

	jobs = kunit_kcalloc(test, ctx->worker_cnt, sizeof(*jobs), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, jobs);

	for (i = 0; i < ctx->worker_cnt; i++)
		jobs[i] = ebr_test_job_start(ctx, i, ebr_test_stress_fn, 0, 0);
	for (i = 0; i < ctx->worker_cnt; i++) {
		ebr_test_job_wait(jobs[i]);
		KUNIT_EXPECT_EQ(test, jobs[i]->wrong, 0);
		KUNIT_EXPECT_GT(test, jobs[i]->result, 0);
	}

	KUNIT_EXPECT_GT(test, atomic_read(&ctx->nodes_freed), 0);
	KUNIT_EXPECT_LE(test, lsv_ht_entry_cnt(ctx->ht), EBR_TEST_STRESS_KEYS);
}

static int ebr_test_init(struct kunit *test)
{
	struct ebr_test_ctx *ctx;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->test = test;
	spin_lock_init(&ctx->quarantine_lock);
	INIT_LIST_HEAD(&ctx->quarantine);
	init_completion(&ctx->parked);
	init_completion(&ctx->release);
	init_completion(&ctx->scan_parked);
	init_completion(&ctx->scan_release);
	/* no key is the tail sentinel's: nothing parks until a case says so */
	ctx->park_key = LSV_HT_KEY_TAIL_SENTINEL;
	test->priv = ctx;

	/* the workers come first: a table counts only those made before it */
	ctx->wq = lsv_shard_wq_create(0, cpu_online_mask, "kunit-ebr");
	if (IS_ERR(ctx->wq)) {
		int rc = PTR_ERR(ctx->wq);

		ctx->wq = NULL;
		return rc;
	}
	ctx->worker_cnt = num_online_cpus();

	WRITE_ONCE(g_ebr_test_ctx, ctx);
	WRITE_ONCE(g_lsv_ht_kunit_cell_check, ebr_test_cell_check);
	WRITE_ONCE(g_lsv_ht_ebr_scanned, ebr_test_scanned);

	return 0;
}

static void ebr_test_exit(struct kunit *test)
{
	struct ebr_test_ctx *ctx = test->priv;
	struct ebr_test_cell *cell;
	struct ebr_test_cell *next;
	unsigned long deadline;

	if (!ctx)
		return;

	/* a case that failed half-way may have left a job parked */
	atomic_set(&ctx->park_armed, 0);
	atomic_set(&ctx->scan_armed, 0);
	complete_all(&ctx->release);
	complete_all(&ctx->scan_release);
	while (atomic_read(&ctx->jobs_running))
		usleep_range(1000, 2000);

	/* the table goes while the workers still run its reclaim requests */
	deadline = jiffies + msecs_to_jiffies(EBR_TEST_WAIT_MS);
	while (ctx->ht && ht_kunit_reclaim_requests_pending(ctx->ht) &&
	       time_before(jiffies, deadline))
		usleep_range(1000, 2000);
	lsv_ht_free(ctx->ht);

	if (ctx->wq)
		lsv_shard_wq_destroy(ctx->wq);
	WRITE_ONCE(g_lsv_ht_ebr_scanned, NULL);
	WRITE_ONCE(g_lsv_ht_kunit_cell_check, NULL);
	WRITE_ONCE(g_ebr_test_ctx, NULL);

	KUNIT_EXPECT_EQ(test, atomic_read(&ctx->nodes_live), 0);
	KUNIT_EXPECT_EQ(test, atomic_read(&ctx->touched_freed), 0);

	list_for_each_entry_safe(cell, next, &ctx->quarantine, qlink)
		kfree(cell);
}

static struct kunit_case g_lsv_ht_ebr_cases[] = {
	LSV_KUNIT_CASE(ebr_basic),
	LSV_KUNIT_CASE(ebr_reclaim_alone),
	LSV_KUNIT_CASE(ebr_stalled_reader),
	LSV_KUNIT_CASE(ebr_idle_participant_garbage),
	LSV_KUNIT_CASE(ebr_out_of_cells),
	LSV_KUNIT_CASE(ebr_stale_advancer),
	LSV_KUNIT_CASE_SLOW(ebr_stress),
	{}
};

static struct kunit_suite g_lsv_ht_ebr_suite = {
#ifdef LSV_HT_EBR_GLOBAL
	.name = "lsv_ht_ebr_global",
#else
	.name = "lsv_ht_ebr_local",
#endif
	.init = ebr_test_init,
	.exit = ebr_test_exit,
	.test_cases = g_lsv_ht_ebr_cases,
};

kunit_test_suites(&g_lsv_ht_ebr_suite);
