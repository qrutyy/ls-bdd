#include <linux/completion.h>
#include <linux/jiffies.h>
#include <kunit/test.h>

#include "utils/lock-free/ht_tests/lsv_kunit_ht.h"

/*
 * An insertion parked in node_alloc(), its cell in hand: the operation is
 * under way, its timestamp published, and every cell retired from then on
 * waits for it -- or for the recovery that answers it.
 */
void lsv_ht_test_stall_participant(struct lsv_ht_test_ctx *ctx, struct lsv_ht_test_job *job,
				   u64 key)
{
	init_completion(&ctx->parked);
	init_completion(&ctx->release);
	atomic_set(&ctx->park_in_alloc, 1);
	job->key = key;
	lsv_ht_test_job_start(ctx, job, 0, 1, lsv_ht_test_insert_fn);
	KUNIT_ASSERT_TRUE(ctx->test,
			  wait_for_completion_timeout(&ctx->parked,
						      msecs_to_jiffies(LSV_HT_TEST_WAIT_MS)));
}

void lsv_ht_test_resume_participant(struct lsv_ht_test_ctx *ctx, struct lsv_ht_test_job *job)
{
	complete(&ctx->release);
	KUNIT_ASSERT_TRUE(ctx->test, lsv_ht_test_job_wait(job));
	KUNIT_ASSERT_TRUE(ctx->test, job->result);
}

long lsv_ht_test_quiet_ops(const struct lsv_ht_test_ctx *ctx)
{
	unsigned int n = max_t(unsigned int, ctx->ht->debra->nr_threads,
			       LSV_INCR_THRESHOLD);

	return 4L * n * LSV_CHECKS_THRESHOLD;
}
