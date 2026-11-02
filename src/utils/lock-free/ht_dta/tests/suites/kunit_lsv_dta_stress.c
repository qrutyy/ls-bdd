#include <kunit/static_stub.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/prandom.h>
#include <linux/slab.h>
#include <kunit/test.h>

#include "linux/hash.h"

#include "utils/kunit.h"

#include "../lsv_kunit_ht.h"
#include "../include_static/__static_kunit_lsv_ht.h"

#define LSV_HT_TEST_RECOVERY_MS		9000
#define LSV_HT_TEST_RECOVERY_CELLS	131072
#define LSV_HT_TEST_STALL_MS		2500	/* longer than a participant may hold a timestamp */
#define LSV_HT_TEST_STALL_GAP_MS	50	/* participants stall in groups, not one by one */
#define LSV_HT_TEST_STALLS		9	/* stalls in the whole run */
#define LSV_HT_TEST_STALL_MAX		3	/* participants stalled at once */
#define LSV_HT_TEST_RECOVERY_PACE_US	500	/* a stalled participant holds reclamation */

/*
 * Stalls the participant that dropped this anchor, for longer than the table
 * waits before it suspects one. Only the jobs the stub is visible to stall,
 * and they stall in groups: a few of them go down within a moment of each
 * other and stay down together, so their ranges in this one bucket overlap and
 * a recovery of one runs into a range frozen for another. Between the groups
 * everyone works and reclamation gets its turn.
 */
static void ht_test_stall_stub(struct lsv_lf_list_node *anchor)
{
	struct kunit *test = kunit_get_current_test();
	unsigned long now = jiffies;
	struct lsv_ht_test_ctx *ctx;
	unsigned long last;

	if (!test)
		return;

	ctx = test->priv;
	last = (unsigned long)atomic_long_read(&ctx->last_stall);
	if (time_before(now, last + msecs_to_jiffies(LSV_HT_TEST_STALL_GAP_MS)))
		return;

	if ((unsigned long)atomic_long_cmpxchg(&ctx->last_stall, last, now) != last)
		return;

	if (atomic_dec_if_positive(&ctx->stalls_left) < 0)
		return;

	if (atomic_inc_return(&ctx->stalled_now) > LSV_HT_TEST_STALL_MAX) {
		atomic_dec(&ctx->stalled_now);
		return;
	}

	msleep(LSV_HT_TEST_STALL_MS);
	atomic_dec(&ctx->stalled_now);
}

static void ht_test_recovery_stress_fn(struct lsv_ht_test_job *job)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(LSV_HT_TEST_RECOVERY_MS);
	struct lsv_ht_test_ctx *ctx = job->ctx;
	struct lsv_ht_test_value *value;
	bool is_new;
	u64 key;

	while (time_before(jiffies, deadline)) {
		key = ctx->bucket_keys[prandom_u32_state(&job->rnd) % LSV_HT_TEST_RECOVERY_KEYS];

		job->iterations++;
		value = lsv_ht_insert_or_inc_ref(ctx->ht, key, &is_new);
		if (!value) {
			job->result++;
			continue;
		}

		if (value->key != key || (is_new && lsv_ht_test_value_published(ctx, value)))
			job->wrong++;

		if (refcount_dec_and_test(&value->ref))
			lsv_ht_remove(ctx->ht, key);

		/*
		 * A stalled participant holds reclamation for as long as it
		 * stalls, so the pool has to carry everything retired
		 * meanwhile: pace the run to a rate it can carry.
		 */
		usleep_range(LSV_HT_TEST_RECOVERY_PACE_US, 2 * LSV_HT_TEST_RECOVERY_PACE_US);
	}
}

/*
 * Recovery under load, in the one place where it is hardest: every
 * participant works on the same bucket, and half of them keep stalling inside
 * their traversals for longer than the table tolerates. The others have to
 * recover them while working, which means ranges frozen for several stalled
 * participants at once, overlapping, each replaced under the others.
 *
 * Cells run out along the way, which is the point as well: a recovery must
 * come through on the reserve its participant kept back.
 */
static void ht_test_concurrent_recovery(struct kunit *test, int cells, int recovery_cells,
					bool tight, bool reuse)
{
	struct lsv_ht_test_ctx *ctx = test->priv;
	u64 seed = lsv_ht_test_seed_get(test);
	unsigned int n = 0;
	long failed = 0;
	long wrong = 0;
	u64 left = 0;
	struct lsv_ht_test_value *value;
	struct lsv_ht_test_job *jobs;
	unsigned int nr_jobs;
	unsigned int bucket;
	unsigned int i;
	u64 key;

	KUNIT_ASSERT_GE(test, ctx->worker_cnt, 2U);
	nr_jobs = LSV_HT_TEST_WQ_CNT * ctx->worker_cnt;
	ctx->copy_fallback = tight;
	ctx->reuse = reuse;
	KUNIT_ASSERT_EQ(test, lsv_ht_test_create_table(ctx, 4 * LSV_HT_TEST_RECOVERY_KEYS, cells,
						       recovery_cells), 0);

	bucket = hash_64(1, ctx->ht->bucket_bits);
	for (key = 1; n < LSV_HT_TEST_RECOVERY_KEYS; key++) {
		if (hash_64(key, ctx->ht->bucket_bits) == bucket)
			ctx->bucket_keys[n++] = key;
	}

	jobs = kunit_kcalloc(test, nr_jobs, sizeof(*jobs), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, jobs);

	atomic_set(&ctx->stalls_left, LSV_HT_TEST_STALLS);
	kunit_activate_static_stub(test, ht_anchor_dropped, ht_test_stall_stub);

	for (i = 0; i < nr_jobs; i++) {
		/* the stub only stalls the jobs it is visible to */
		jobs[i].stub_visible = i % 2;
		prandom_seed_state(&jobs[i].rnd, seed + i);
		lsv_ht_test_job_start(ctx, &jobs[i], i % LSV_HT_TEST_WQ_CNT,
				      (i / LSV_HT_TEST_WQ_CNT) % ctx->worker_cnt,
				      ht_test_recovery_stress_fn);
	}
	for (i = 0; i < nr_jobs; i++) {
		KUNIT_ASSERT_TRUE(test, lsv_ht_test_job_wait(&jobs[i]));
		failed += jobs[i].result;
		wrong += jobs[i].wrong;
	}

	kunit_deactivate_static_stub(test, ht_anchor_dropped);

	kunit_info(test, "participants=%u failed_ops=%ld recoveries=%llu freed_cells=%d stalls=%d fallback_cells=%d\n",
		   nr_jobs, failed, lsv_ht_recovery_cnt(ctx->ht),
		   atomic_read(&ctx->nodes_freed),
		   LSV_HT_TEST_STALLS - atomic_read(&ctx->stalls_left),
		   atomic_read(&ctx->fallback_cnt));

	/*
	 * A value is never the wrong one, nor one of two live for its key,
	 * whatever the recoveries did to its cells.
	 */
	KUNIT_EXPECT_EQ_MSG(test, wrong, 0L, "seed=%llu", seed);
	/* the scenario has to have happened: participants stalled, and were recovered */
	KUNIT_EXPECT_GE(test, LSV_HT_TEST_STALLS - atomic_read(&ctx->stalls_left), 4);
	KUNIT_EXPECT_GT(test, lsv_ht_recovery_cnt(ctx->ht), 0ULL);
	/* the run is paced so that a roomy pool carries it: failures stay the exception */
	if (!tight)
		KUNIT_EXPECT_LT_MSG(test, failed, (long)(nr_jobs * 1000), "seed=%llu", seed);
	/* every pin was put, and no value lost its last cell while pinned */
	KUNIT_EXPECT_EQ_MSG(test, lsv_ht_test_values_held(ctx), 0U, "seed=%llu", seed);
	KUNIT_EXPECT_EQ_MSG(test, atomic_read(&ctx->released_held), 0, "seed=%llu", seed);

	for (n = 0; n < LSV_HT_TEST_RECOVERY_KEYS; n++) {
		value = lsv_ht_test_find(ctx, ctx->bucket_keys[n]);
		if (!value)
			continue;
		left++;
		KUNIT_EXPECT_EQ(test, value->key, ctx->bucket_keys[n]);
	}

	kunit_info(test, "keys left: %llu\n", left);
	KUNIT_EXPECT_EQ(test, lsv_ht_entry_cnt(ctx->ht), left);

	/*
	 * The table still works once nobody stalls any more, a tight pool
	 * included: the cells idle participants keep in their buffers come
	 * back on request, if not for the insertion that asked for them.
	 */
	KUNIT_EXPECT_NOT_NULL(test, lsv_ht_test_insert_eventually(ctx, ctx->bucket_keys[0]));
}

static void LSV_KTEST(ht_concurrent_recovery)(struct kunit *test)
{
	ht_test_concurrent_recovery(test, LSV_HT_TEST_RECOVERY_CELLS, LSV_HT_TEST_RECOVERY_RESERVE,
				    false, false);
}

/*
 * The same with the pool just above the keys and no more than
 * LSV_DTA_SPARE_CELLS kept for node_copy(): the pool runs dry at every stall,
 * insertions fail, and the recoveries of overlapping ranges have to come
 * through on the reserve and the fallback of node_copy(). Failures are
 * expected; what may not happen is a wrong value, two live values of a key, a
 * value that loses its last cell while held, or a walk onto a freed cell.
 */
static void LSV_KTEST(ht_concurrent_recovery_tight_pool)(struct kunit *test)
{
	ht_test_concurrent_recovery(test, 4 * LSV_HT_TEST_RECOVERY_KEYS, LSV_DTA_SPARE_CELLS,
				    true, false);
}

/* Recovery under load with cells handed out again as soon as they come back. */
static void LSV_KTEST(ht_concurrent_recovery_reused_cells)(struct kunit *test)
{
	struct lsv_ht_test_ctx *ctx = test->priv;

	ht_test_concurrent_recovery(test, LSV_HT_TEST_RECOVERY_CELLS, LSV_HT_TEST_RECOVERY_RESERVE,
				    false, true);
	kunit_info(test, "cells reused: %d\n", atomic_read(&ctx->reused));
	KUNIT_EXPECT_GT(test, atomic_read(&ctx->reused), 0);
}

static struct kunit_case g_lsv_ht_stress_cases[] = {
	LSV_KUNIT_CASE_SLOW(ht_concurrent_recovery),
	LSV_KUNIT_CASE_SLOW(ht_concurrent_recovery_tight_pool),
	LSV_KUNIT_CASE_SLOW(ht_concurrent_recovery_reused_cells),
	{}
};

static struct kunit_suite g_lsv_ht_stress_suite = {
	.name = "lsv_ht_dta_stress",
	.init = lsv_ht_test_init,
	.exit = lsv_ht_test_exit,
	.test_cases = g_lsv_ht_stress_cases,
};

kunit_test_suites(&g_lsv_ht_stress_suite);
