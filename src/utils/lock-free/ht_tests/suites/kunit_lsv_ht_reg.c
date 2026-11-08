#include <kunit/static_stub.h>
#include <kunit/test.h>
#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/cpumask.h>
#include <linux/jiffies.h>
#include <linux/list.h>
#include <linux/prandom.h>
#include <linux/spinlock.h>

#include "utils/wq.h"
#include "utils/kunit.h"

#include "../lsv_kunit_ht.h"

/*
 * What every lsv_ht implementation owes its consumer, whatever it reclaims
 * cells with: the API and value lifetime contract, the pool of cells, the
 * participants of the table, concurrent use, and freeing the table. Each
 * implementation adds the cases of its own mechanism in its tests/suites.
 */

static void LSV_KTEST(ht_insert_find_remove)(struct kunit *test)
{
	struct lsv_ht_test_ctx *ctx = test->priv;
	struct lsv_ht_test_value *value;
	bool is_new;

	KUNIT_ASSERT_EQ(test, lsv_ht_test_create_table(ctx, 64, 64), 0);

	/* A new key gets a fresh value holding one reference. */
	value = lsv_ht_test_insert(ctx, 42, &is_new);
	KUNIT_ASSERT_NOT_NULL(test, value);
	KUNIT_EXPECT_TRUE(test, is_new);
	KUNIT_EXPECT_EQ(test, value->key, 42ULL);
	KUNIT_EXPECT_EQ(test, refcount_read(&value->ref), 1U);
	KUNIT_EXPECT_EQ(test, lsv_ht_entry_cnt(ctx->ht), 1ULL);

	/* The same key returns the same value, pinned once more. */
	KUNIT_EXPECT_PTR_EQ(test, lsv_ht_test_insert(ctx, 42, &is_new), value);
	KUNIT_EXPECT_FALSE(test, is_new);
	KUNIT_EXPECT_EQ(test, refcount_read(&value->ref), 2U);
	KUNIT_EXPECT_EQ(test, lsv_ht_entry_cnt(ctx->ht), 1ULL);

	/* Lookup takes no reference. */
	KUNIT_EXPECT_PTR_EQ(test, lsv_ht_test_find(ctx, 42), value);
	KUNIT_EXPECT_EQ(test, refcount_read(&value->ref), 2U);
	KUNIT_EXPECT_NULL(test, lsv_ht_test_find(ctx, 43));

	/* A value still referenced keeps its key: only the last put removes it. */
	KUNIT_EXPECT_FALSE(test, lsv_ht_test_remove(ctx, 42));
	KUNIT_EXPECT_PTR_EQ(test, lsv_ht_test_find(ctx, 42), value);
	KUNIT_EXPECT_FALSE(test, refcount_dec_and_test(&value->ref));

	KUNIT_EXPECT_TRUE(test, lsv_ht_test_release_key(ctx, 42));
	KUNIT_EXPECT_FALSE(test, lsv_ht_test_remove(ctx, 42));
	KUNIT_EXPECT_NULL(test, lsv_ht_test_find(ctx, 42));
	KUNIT_EXPECT_EQ(test, lsv_ht_entry_cnt(ctx->ht), 0ULL);
	KUNIT_EXPECT_TRUE(test, lsv_ht_is_empty(ctx->ht));

	/* Reserved sentinel keys are never stored. */
	KUNIT_EXPECT_NULL(test, lsv_ht_test_insert(ctx, LSV_HT_KEY_HEAD_SENTINEL, &is_new));
	KUNIT_EXPECT_NULL(test, lsv_ht_test_insert(ctx, LSV_HT_KEY_TAIL_SENTINEL, &is_new));
}

#define LSV_HT_TEST_CAPACITY	8

static void LSV_KTEST(ht_capacity)(struct kunit *test)
{
	struct lsv_ht_test_ctx *ctx = test->priv;
	bool is_new;
	u64 key;

	KUNIT_ASSERT_EQ(test, lsv_ht_test_create_table(ctx, LSV_HT_TEST_CAPACITY, 64), 0);

	/* the size of the table, whatever the number of CPUs */
	for (key = 1; key <= LSV_HT_TEST_CAPACITY; key++)
		KUNIT_ASSERT_NOT_NULL(test, lsv_ht_test_insert(ctx, key, &is_new));
	KUNIT_EXPECT_TRUE(test, lsv_ht_is_full(ctx->ht));

	/* A full table refuses new keys but still serves existing ones. */
	KUNIT_EXPECT_NULL(test, lsv_ht_test_insert(ctx, LSV_HT_TEST_CAPACITY + 1, &is_new));
	KUNIT_EXPECT_NOT_NULL(test, lsv_ht_test_insert(ctx, LSV_HT_TEST_CAPACITY, &is_new));
	KUNIT_EXPECT_FALSE(test, is_new);
	KUNIT_EXPECT_EQ(test, lsv_ht_entry_cnt(ctx->ht), (u64)LSV_HT_TEST_CAPACITY);

	/* Removing a key frees its slot. */
	KUNIT_EXPECT_TRUE(test, lsv_ht_test_release_key(ctx, 1));
	KUNIT_EXPECT_NOT_NULL(test, lsv_ht_test_insert(ctx, LSV_HT_TEST_CAPACITY + 1, &is_new));
	KUNIT_EXPECT_TRUE(test, is_new);
}

/*
 * The table is sized in entries, but it runs on a pool of cells, and the two
 * run out separately: a key the pool cannot carry is refused although the
 * table has room for it.
 *
 * What a refusal must not cost is the table itself: the entry a failed
 * insertion reserved has to go back, an existing key has to stay reachable
 * with no cell to spare, and the cells of removed keys have to come back once
 * the reclamation bounds let them, which is what makes the pool a pool rather
 * than a budget spent once.
 */
static void LSV_KTEST(ht_cell_exhaustion)(struct kunit *test)
{
	struct lsv_ht_test_ctx *ctx = test->priv;
	bool is_new;
	u64 key;

	KUNIT_ASSERT_EQ(test, lsv_ht_test_create_table(ctx, 64, 4), 0);

	for (key = 1; key <= 4; key++)
		KUNIT_ASSERT_NOT_NULL(test, lsv_ht_test_insert(ctx, key, &is_new));

	/* No cell left: the insert fails and leaves the count untouched. */
	KUNIT_EXPECT_NULL(test, lsv_ht_test_insert(ctx, 5, &is_new));
	KUNIT_EXPECT_EQ(test, lsv_ht_entry_cnt(ctx->ht), 4ULL);

	/* An existing key is still pinned with no cell to spare. */
	KUNIT_EXPECT_NOT_NULL(test, lsv_ht_test_insert(ctx, 4, &is_new));
	KUNIT_EXPECT_FALSE(test, is_new);

	/*
	 * Removed cells come back once every later operation has started
	 * after their retirement: keep operating until one is reclaimed.
	 */
	for (key = 1; key <= 3; key++)
		KUNIT_EXPECT_TRUE(test, lsv_ht_test_release_key(ctx, key));
	for (key = 0; key < 8 && !atomic_read(&ctx->nodes_freed); key++)
		lsv_ht_test_find(ctx, 4);
	KUNIT_EXPECT_GT(test, atomic_read(&ctx->nodes_freed), 0);
	KUNIT_EXPECT_NOT_NULL(test, lsv_ht_test_insert(ctx, 5, &is_new));
	KUNIT_EXPECT_TRUE(test, is_new);
}

/*
 * A remove may fail spuriously, and its caller has no reference left to retry
 * with: the key must not stay behind as one that can never be pinned and never
 * be inserted again.
 */
static void LSV_KTEST(ht_dead_entry_reuse)(struct kunit *test)
{
	struct lsv_ht_test_ctx *ctx = test->priv;
	struct lsv_ht_test_value *value;
	struct lsv_ht_test_value *dead;
	bool is_new;

	KUNIT_ASSERT_EQ(test, lsv_ht_test_create_table(ctx, 64, 64), 0);

	dead = lsv_ht_test_insert(ctx, 7, &is_new);
	KUNIT_ASSERT_NOT_NULL(test, dead);
	KUNIT_ASSERT_TRUE(test, is_new);

	/* the last put, with the remove that should have followed it lost */
	KUNIT_ASSERT_TRUE(test, refcount_dec_and_test(&dead->ref));

	/* a value nobody holds any more is handed to nobody, and unpublished */
	KUNIT_EXPECT_NULL(test, lsv_ht_test_find(ctx, 7));
	KUNIT_EXPECT_EQ(test, lsv_ht_entry_cnt(ctx->ht), 0ULL);

	value = lsv_ht_test_insert(ctx, 7, &is_new);
	KUNIT_ASSERT_NOT_NULL(test, value);
	KUNIT_EXPECT_TRUE(test, is_new);
	KUNIT_EXPECT_PTR_NE(test, (void *)value, (void *)dead);
	KUNIT_EXPECT_EQ(test, refcount_read(&value->ref), 1U);
	KUNIT_EXPECT_EQ(test, lsv_ht_entry_cnt(ctx->ht), 1ULL);

	KUNIT_EXPECT_PTR_EQ(test, lsv_ht_test_find(ctx, 7), (void *)value);
	KUNIT_EXPECT_TRUE(test, lsv_ht_test_release_key(ctx, 7));
	KUNIT_EXPECT_EQ(test, lsv_ht_entry_cnt(ctx->ht), 0ULL);
}

/*
 * The remove that follows a last put may come after the key has moved on: the
 * dead entry dropped by another operation, and the key inserted anew. That
 * remove must leave the new value alone. It is someone else's, and a key taken
 * away from under it would let the next insertion create a second live value
 * for the same key.
 */
static void LSV_KTEST(ht_late_remove_spares_new_value)(struct kunit *test)
{
	struct lsv_ht_test_ctx *ctx = test->priv;
	struct lsv_ht_test_value *value;
	struct lsv_ht_test_value *old;
	bool is_new;

	KUNIT_ASSERT_EQ(test, lsv_ht_test_create_table(ctx, 64, 64), 0);

	old = lsv_ht_test_insert(ctx, 7, &is_new);
	KUNIT_ASSERT_NOT_NULL(test, old);

	/* Arrange: the last put of the old value, its remove still to come */
	KUNIT_ASSERT_TRUE(test, refcount_dec_and_test(&old->ref));

	/* another request drops the dead entry and inserts the key anew */
	value = lsv_ht_test_insert(ctx, 7, &is_new);
	KUNIT_ASSERT_NOT_NULL(test, value);
	KUNIT_ASSERT_TRUE(test, is_new);
	KUNIT_ASSERT_PTR_NE(test, (void *)value, (void *)old);

	/* Act: the late remove of the old value */
	KUNIT_EXPECT_FALSE(test, lsv_ht_test_remove(ctx, 7));

	/* Assert: the new value keeps its key, and the next request pins it */
	KUNIT_EXPECT_PTR_EQ(test, lsv_ht_test_find(ctx, 7), (void *)value);
	KUNIT_EXPECT_PTR_EQ(test, lsv_ht_test_insert(ctx, 7, &is_new), (void *)value);
	KUNIT_EXPECT_FALSE(test, is_new);
	KUNIT_EXPECT_EQ(test, refcount_read(&value->ref), 2U);
	KUNIT_EXPECT_EQ(test, lsv_ht_entry_cnt(ctx->ht), 1ULL);
}

/*
 * An insertion that loses the race for its key comes back with the value that
 * won, and the value it made for itself was never anyone's: the table drops
 * the reference that value was born with and gives its cell back, so that the
 * value goes with its last cell and not while anything still counts on it.
 */
static void LSV_KTEST(ht_lost_insertion_drops_its_value)(struct kunit *test)
{
	struct lsv_ht_test_job *loser = lsv_ht_test_job_new(test);
	struct lsv_ht_test_ctx *ctx = test->priv;
	struct lsv_ht_test_value *made = NULL;
	struct lsv_ht_test_value *value;
	struct lsv_ht_test_value *v;
	bool is_new;

	KUNIT_ASSERT_EQ(test, lsv_ht_test_create_table(ctx, 64, 64), 0);

	/* Arrange: an insertion of the key parks with its value and cell made */
	init_completion(&ctx->link_parked);
	init_completion(&ctx->link_release);
	atomic_set(&ctx->link_armed, 1);
	kunit_activate_static_stub(test, ht_kunit_cell_linking, lsv_ht_test_cell_linking_stub);
	loser->key = 7;
	loser->stub_visible = true;
	lsv_ht_test_job_start(ctx, loser, 1, 0, lsv_ht_test_insert_fn);
	KUNIT_ASSERT_TRUE(test, wait_for_completion_timeout(&ctx->link_parked,
							    msecs_to_jiffies(LSV_HT_TEST_WAIT_MS)));

	/* another insertion of the same key links first */
	value = lsv_ht_test_insert(ctx, 7, &is_new);
	KUNIT_ASSERT_NOT_NULL(test, value);
	KUNIT_ASSERT_TRUE(test, is_new);

	/* Act: the parked insertion resumes and loses its link */
	complete(&ctx->link_release);
	KUNIT_ASSERT_TRUE(test, lsv_ht_test_job_wait(loser));
	kunit_deactivate_static_stub(test, ht_kunit_cell_linking);

	/* Assert: it pinned the value that won... */
	KUNIT_EXPECT_PTR_EQ(test, loser->value, (void *)value);
	KUNIT_EXPECT_FALSE(test, loser->result);
	KUNIT_EXPECT_EQ(test, refcount_read(&value->ref), 2U);
	KUNIT_EXPECT_EQ(test, lsv_ht_entry_cnt(ctx->ht), 1ULL);

	/* ...and what it made is gone: no reference, no cell */
	spin_lock(&ctx->values_lock);
	list_for_each_entry(v, &ctx->values, link) {
		if (v->key == 7 && v != value)
			made = v;
	}
	spin_unlock(&ctx->values_lock);
	KUNIT_ASSERT_NOT_NULL(test, made);
	KUNIT_EXPECT_EQ(test, refcount_read(&made->ref), 0U);
	KUNIT_EXPECT_EQ(test, atomic_read(&made->cell_cnt), 0);
}

/*
 * A pool of two cells, and two removals, fewer than the three retirements
 * after which a participant scans its retire buffer: the cells of the removed
 * keys are there to be given back, and an insertion the allocator turns away
 * has to get one of them back rather than fail while it sits in the buffer.
 */
static void LSV_KTEST(ht_cell_shortage_reclaims)(struct kunit *test)
{
	struct lsv_ht_test_ctx *ctx = test->priv;
	struct lsv_ht_test_value *value;
	bool is_new;
	u64 key;

	/* far from the fill at which every operation scans its buffer */
	KUNIT_ASSERT_EQ(test, lsv_ht_test_create_table(ctx, 64, 2), 0);

	for (key = 1; key <= 2; key++)
		KUNIT_ASSERT_NOT_NULL(test, lsv_ht_test_insert(ctx, key, &is_new));
	for (key = 1; key <= 2; key++)
		KUNIT_ASSERT_TRUE(test, lsv_ht_test_release_key(ctx, key));

	/* Arrange: both cells retired, none scanned yet, the pool dry */
	KUNIT_ASSERT_EQ(test, atomic_read(&ctx->nodes_freed), 0);
	KUNIT_ASSERT_TRUE(test, lsv_ht_is_empty(ctx->ht));

	/* Act */
	value = lsv_ht_test_insert(ctx, 3, &is_new);

	/* Assert */
	KUNIT_EXPECT_NOT_NULL(test, value);
	KUNIT_EXPECT_TRUE(test, is_new);
	KUNIT_EXPECT_GT(test, atomic_read(&ctx->nodes_freed), 0);
}

/*
 * Without value_get() the table knows nothing of references: it pins
 * nothing, drops no entry as dead, and removes a key whatever it holds.
 */
static void LSV_KTEST(ht_values_without_refs)(struct kunit *test)
{
	struct lsv_ht_test_ctx *ctx = test->priv;
	struct lsv_ht_test_value *value;
	struct lsv_ht_test_value *again;
	bool is_new;

	KUNIT_ASSERT_EQ(test,
			lsv_ht_test_create_table_ops(ctx, 64, 64, &g_lsv_ht_test_ops_norefs), 0);

	value = lsv_ht_test_insert(ctx, 7, &is_new);
	KUNIT_ASSERT_NOT_NULL(test, value);
	KUNIT_EXPECT_TRUE(test, is_new);

	/* the key is found and no reference is taken for it */
	KUNIT_EXPECT_PTR_EQ(test, lsv_ht_test_insert(ctx, 7, &is_new), (void *)value);
	KUNIT_EXPECT_FALSE(test, is_new);
	KUNIT_EXPECT_EQ(test, refcount_read(&value->ref), 1U);

	/* a zero count means nothing to it either: the entry stays */
	refcount_set(&value->ref, 0);
	KUNIT_EXPECT_PTR_EQ(test, lsv_ht_test_find(ctx, 7), (void *)value);
	KUNIT_EXPECT_EQ(test, lsv_ht_entry_cnt(ctx->ht), 1ULL);
	refcount_set(&value->ref, 1);

	/* and remove takes the key whatever it holds */
	KUNIT_EXPECT_TRUE(test, lsv_ht_test_remove(ctx, 7));
	KUNIT_EXPECT_FALSE(test, lsv_ht_test_remove(ctx, 7));
	KUNIT_EXPECT_NULL(test, lsv_ht_test_find(ctx, 7));
	KUNIT_EXPECT_EQ(test, lsv_ht_entry_cnt(ctx->ht), 0ULL);

	again = lsv_ht_test_insert(ctx, 7, &is_new);
	KUNIT_EXPECT_NOT_NULL(test, again);
	KUNIT_EXPECT_TRUE(test, is_new);
	KUNIT_EXPECT_PTR_NE(test, (void *)again, (void *)value);
}

static void ht_test_wq_destroy(void *wq)
{
	lsv_wq_destroy(wq);
}

/*
 * The table sizes its participants when it is created: a worker of a queue
 * created later is not one of them, and every operation it tries fails
 * without touching the table.
 */
static void LSV_KTEST(ht_late_worker_refused)(struct kunit *test)
{
	struct lsv_ht_test_job *job = lsv_ht_test_job_new(test);
	struct lsv_ht_test_ctx *ctx = test->priv;
	struct lsv_ht_test_value *value;
	struct lsv_wq *late;
	bool is_new;

	KUNIT_ASSERT_EQ(test, lsv_ht_test_create_table(ctx, 64, 64), 0);
	value = lsv_ht_test_insert(ctx, 5, &is_new);
	KUNIT_ASSERT_NOT_NULL(test, value);

	late = lsv_wq_create(1, cpu_online_mask, "kunit-ht-late");
	KUNIT_ASSERT_FALSE(test, IS_ERR(late));
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, ht_test_wq_destroy, late), 0);

	job->key = 5;
	lsv_ht_test_job_start_on(ctx, job, lsv_wq_map(late, NULL, cpumask_first(cpu_online_mask)),
				 lsv_ht_test_insert_fn);
	KUNIT_ASSERT_TRUE(test, lsv_ht_test_job_wait(job));
	KUNIT_EXPECT_NULL(test, job->value);
	KUNIT_EXPECT_FALSE(test, job->result);

	job = lsv_ht_test_job_new(test);
	job->key = 5;
	lsv_ht_test_job_start_on(ctx, job, lsv_wq_map(late, NULL, cpumask_first(cpu_online_mask)),
				 lsv_ht_test_remove_fn);
	KUNIT_ASSERT_TRUE(test, lsv_ht_test_job_wait(job));
	KUNIT_EXPECT_FALSE(test, job->result);

	/* nothing changed for the participants */
	KUNIT_EXPECT_EQ(test, refcount_read(&value->ref), 1U);
	KUNIT_EXPECT_PTR_EQ(test, lsv_ht_test_find(ctx, 5), (void *)value);
	KUNIT_EXPECT_EQ(test, lsv_ht_entry_cnt(ctx->ht), 1ULL);
}

#define LSV_HT_TEST_ADMIT_SIZE		256
#define LSV_HT_TEST_ADMIT_PER_JOB	128

/* Inserts keys of its own, all distinct from every other job's. */
static void ht_test_admit_fn(struct lsv_ht_test_job *job)
{
	struct lsv_ht_test_value *value;
	unsigned int i;
	bool is_new;

	for (i = 0; i < LSV_HT_TEST_ADMIT_PER_JOB; i++) {
		value = lsv_ht_insert_or_inc_ref(job->ctx->ht, job->key + i, &is_new);
		if (value && is_new)
			job->result++;
		else if (value)
			job->wrong++;
	}
}

/*
 * The size of the table is a hard limit however many insertions race for the
 * last places, and none is lost either: with cells to spare and every key
 * distinct, exactly as many insertions succeed as the table has room for.
 */
static void LSV_KTEST(ht_capacity_under_contention)(struct kunit *test)
{
	struct lsv_ht_test_ctx *ctx = test->priv;
	unsigned int nr_jobs = LSV_HT_TEST_WQ_CNT * ctx->worker_cnt;
	long admitted = 0;
	long wrong = 0;
	struct lsv_ht_test_job *jobs;
	unsigned int i;

	KUNIT_ASSERT_GT(test, nr_jobs * LSV_HT_TEST_ADMIT_PER_JOB,
			(unsigned int)LSV_HT_TEST_ADMIT_SIZE);
	KUNIT_ASSERT_EQ(test, lsv_ht_test_create_table(ctx, LSV_HT_TEST_ADMIT_SIZE,
						       nr_jobs * LSV_HT_TEST_ADMIT_PER_JOB), 0);

	jobs = kunit_kcalloc(test, nr_jobs, sizeof(*jobs), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, jobs);
	for (i = 0; i < nr_jobs; i++) {
		jobs[i].key = 1 + (u64)i * LSV_HT_TEST_ADMIT_PER_JOB;
		lsv_ht_test_job_start(ctx, &jobs[i], i % LSV_HT_TEST_WQ_CNT,
				      (i / LSV_HT_TEST_WQ_CNT) % ctx->worker_cnt, ht_test_admit_fn);
	}
	for (i = 0; i < nr_jobs; i++) {
		KUNIT_ASSERT_TRUE(test, lsv_ht_test_job_wait(&jobs[i]));
		admitted += jobs[i].result;
		wrong += jobs[i].wrong;
	}

	KUNIT_EXPECT_EQ(test, wrong, 0L);
	KUNIT_EXPECT_EQ(test, admitted, (long)LSV_HT_TEST_ADMIT_SIZE);
	KUNIT_EXPECT_EQ(test, lsv_ht_entry_cnt(ctx->ht), (u64)LSV_HT_TEST_ADMIT_SIZE);
	/* nothing but the admitted entries holds a cell */
	KUNIT_EXPECT_EQ(test, atomic_read(&ctx->nodes_live), LSV_HT_TEST_ADMIT_SIZE);
}

#define LSV_HT_TEST_STRESS_KEYS		64
#define LSV_HT_TEST_STRESS_ITERS	2000
/* cells of the pool per job, an eighth of what a job may retire */
#define LSV_HT_TEST_POOL_PER_JOB	256

/*
 * Pin a random key, then drop the pin; the last put removes the key, as the
 * value lifetime contract requires.
 */
static void ht_test_stress_fn(struct lsv_ht_test_job *job)
{
	struct lsv_ht_test_ctx *ctx = job->ctx;
	struct lsv_ht_test_value *value;
	unsigned int i;
	bool is_new;
	u64 key;

	for (i = 0; i < job->iterations; i++) {
		key = 1 + prandom_u32_state(&job->rnd) % LSV_HT_TEST_STRESS_KEYS;

		value = lsv_ht_insert_or_inc_ref(ctx->ht, key, &is_new);
		if (!value) {
			job->result++;
			continue;
		}

		if (value->key != key || (is_new && lsv_ht_test_value_published(ctx, value)))
			job->wrong++;

		if (refcount_dec_and_test(&value->ref))
			lsv_ht_remove(ctx->ht, key);
	}
}

static void ht_test_stress_check_fn(struct lsv_ht_test_job *job)
{
	struct lsv_ht_test_ctx *ctx = job->ctx;
	struct lsv_ht_test_value *value;
	u64 key;

	for (key = 1; key <= LSV_HT_TEST_STRESS_KEYS; key++) {
		value = lsv_ht_find_value(ctx->ht, key);
		if (!value)
			continue;
		job->iterations++;
		/* a present key may only be one whose last put lost its remove */
		if (value->key != key || refcount_read(&value->ref))
			job->result++;
	}
}

/* More participants than the original fixed eight slots, on two wqs. */
static void ht_test_concurrent_participants(struct kunit *test, bool reuse)
{
	struct lsv_ht_test_job *check = lsv_ht_test_job_new(test);
	struct lsv_ht_test_ctx *ctx = test->priv;
	unsigned int nr_jobs = LSV_HT_TEST_WQ_CNT * ctx->worker_cnt;
	u64 seed = lsv_ht_test_seed_get(test);
	long failed = 0;
	long wrong = 0;
	struct lsv_ht_test_job *jobs;
	unsigned int i;

	ctx->reuse = reuse;
	/*
	 * Cells wait in retire buffers while other participants run, and two
	 * workers share every CPU here: a pool of a few hundred cells per job
	 * carries the run, a small part of what it retires.
	 */
	KUNIT_ASSERT_EQ(test, lsv_ht_test_create_table_reserve(ctx, 4 * LSV_HT_TEST_STRESS_KEYS,
							       4 * LSV_HT_TEST_STRESS_KEYS +
							       LSV_HT_TEST_POOL_PER_JOB * nr_jobs,
							       LSV_HT_TEST_STRESS_RESERVE), 0);

	jobs = kunit_kcalloc(test, nr_jobs, sizeof(*jobs), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, jobs);

	for (i = 0; i < nr_jobs; i++) {
		jobs[i].iterations = LSV_HT_TEST_STRESS_ITERS;
		prandom_seed_state(&jobs[i].rnd, seed + i);
		lsv_ht_test_job_start(ctx, &jobs[i], i % LSV_HT_TEST_WQ_CNT,
				      (i / LSV_HT_TEST_WQ_CNT) % ctx->worker_cnt,
				      ht_test_stress_fn);
	}
	for (i = 0; i < nr_jobs; i++) {
		KUNIT_ASSERT_TRUE(test, lsv_ht_test_job_wait(&jobs[i]));
		failed += jobs[i].result;
		wrong += jobs[i].wrong;
	}

	kunit_info(test, "participants=%u failed_ops=%ld freed_cells=%d\n",
		   nr_jobs, failed, atomic_read(&ctx->nodes_freed));

	/* a value is never the wrong one, nor one of two live for its key */
	KUNIT_EXPECT_EQ_MSG(test, wrong, 0L, "seed=%llu", seed);
	/* spurious failures are allowed, but must stay rare */
	KUNIT_EXPECT_LT_MSG(test, failed, (long)(nr_jobs * LSV_HT_TEST_STRESS_ITERS / 10),
			    "seed=%llu", seed);
	KUNIT_EXPECT_GT(test, nr_jobs, num_online_cpus());
	/* every pin was put, and no value lost its last cell while pinned */
	KUNIT_EXPECT_EQ_MSG(test, lsv_ht_test_values_held(ctx), 0U, "seed=%llu", seed);
	KUNIT_EXPECT_EQ_MSG(test, atomic_read(&ctx->released_held), 0, "seed=%llu", seed);

	lsv_ht_test_run(ctx, check, ht_test_stress_check_fn);
	kunit_info(test, "keys left: %u\n", check->iterations);
	KUNIT_EXPECT_EQ_MSG(test, check->result, 0L, "seed=%llu", seed);
	KUNIT_EXPECT_EQ(test, lsv_ht_entry_cnt(ctx->ht), (u64)check->iterations);
}

static void LSV_KTEST(ht_concurrent_participants)(struct kunit *test)
{
	ht_test_concurrent_participants(test, false);
}

/*
 * The same with cells handed out again as soon as they are given back, so that
 * an address a walk still holds may carry another key, value and next pointer.
 */
static void LSV_KTEST(ht_concurrent_participants_reused_cells)(struct kunit *test)
{
	struct lsv_ht_test_ctx *ctx = test->priv;

	ht_test_concurrent_participants(test, true);
	kunit_info(test, "cells reused: %d\n", atomic_read(&ctx->reused));
	KUNIT_EXPECT_GT(test, atomic_read(&ctx->reused), 0);
}

/* Inserts and releases keys of its own until the pool turns it away. */
static void ht_test_drain_pool_fn(struct lsv_ht_test_job *job)
{
	struct lsv_ht_test_value *value;
	unsigned int i;
	bool is_new;

	for (i = 0; i < 64; i++) {
		value = lsv_ht_insert_or_inc_ref(job->ctx->ht, job->key + i, &is_new);
		if (!value)
			break;
		job->result++;
		if (refcount_dec_and_test(&value->ref))
			lsv_ht_remove(job->ctx->ht, job->key + i);
	}
}

/*
 * The cells a participant retired while another one stalled stay in its own
 * buffer, and that participant may never come back to the table. Once the
 * stall is over, an insertion that finds the pool dry has to get them back
 * all the same: not for itself, it has run out of cells, but for the ones
 * after it.
 */
static void ht_test_idle_buffer(struct lsv_ht_test_ctx *ctx)
{
	struct kunit *test = ctx->test;
	struct lsv_ht_test_job *stalled = lsv_ht_test_job_new(ctx->test);
	struct lsv_ht_test_job *drain = lsv_ht_test_job_new(ctx->test);

	KUNIT_ASSERT_GE(test, ctx->worker_cnt, 2U);
	KUNIT_ASSERT_EQ(test, lsv_ht_test_create_table(ctx, 64, 8), 0);

	/* worker 1 of wq 0 stalls in the middle of an operation, its cell in hand... */
	lsv_ht_test_stall_participant(ctx, stalled, 100);

	/* ...worker 0 of wq 1 churns the rest of the pool into its buffer, and goes idle */
	drain->key = 200;
	lsv_ht_test_job_start(ctx, drain, 1, 0, ht_test_drain_pool_fn);
	KUNIT_ASSERT_TRUE(test, lsv_ht_test_job_wait(drain));
	KUNIT_ASSERT_GE(test, drain->result, 3L);
	KUNIT_ASSERT_EQ(test, atomic_read(&ctx->nodes_freed), 0);

	/* ...and the stall ends */
	lsv_ht_test_resume_participant(ctx, stalled);
}

static void LSV_KTEST(ht_idle_buffer_comes_back)(struct kunit *test)
{
	struct lsv_ht_test_ctx *ctx = test->priv;
	bool is_new;

	/* Arrange: worker 0 of wq 1 keeps a buffer, and nothing else can come back */
	ht_test_idle_buffer(ctx);

	/* Act: worker 0 of wq 0 finds the pool dry, and its own buffer empty */
	KUNIT_EXPECT_NULL(test, lsv_ht_test_insert(ctx, 300, &is_new));

	/* Assert: the idle buffer comes back, and insertions succeed again */
	KUNIT_EXPECT_NOT_NULL(test, lsv_ht_test_insert_eventually(ctx, 300));
	KUNIT_EXPECT_GT(test, atomic_read(&ctx->nodes_freed), 0);
}

/* Holds its worker until the test lets it go. */
static void ht_test_hold_fn(struct lsv_ht_test_job *job)
{
	complete(&job->ctx->hold_parked);
	wait_for_completion(&job->ctx->hold_release);
}

static void ht_test_free_fn(struct lsv_ht_test_job *job)
{
	lsv_ht_free(job->ctx->ht);
	job->ctx->ht = NULL;
}

static void ht_test_hold_then_free_fn(struct lsv_ht_test_job *job)
{
	ht_test_hold_fn(job);
	ht_test_free_fn(job);
}

/*
 * Leaves a reclaim request queued on worker 0 of wq 1 behind a job that holds
 * the worker, |fn| on it.
 */
static void ht_test_request_behind(struct lsv_ht_test_ctx *ctx, struct lsv_ht_test_job *hold,
				   void (*fn)(struct lsv_ht_test_job *))
{
	struct kunit *test = ctx->test;
	bool is_new;

	ht_test_idle_buffer(ctx);

	lsv_ht_test_job_start(ctx, hold, 1, 0, fn);
	KUNIT_ASSERT_TRUE(test, wait_for_completion_timeout(&ctx->hold_parked,
							    msecs_to_jiffies(LSV_HT_TEST_WAIT_MS)));
	KUNIT_ASSERT_NULL(test, lsv_ht_test_insert(ctx, 300, &is_new));
	KUNIT_ASSERT_TRUE(test, ht_kunit_reclaim_requests_pending(ctx->ht));

	/* the table goes with the entries still in it */
	WRITE_ONCE(ctx->tearing_down, true);
}

/*
 * The table is freed while a reclaim request is queued on a worker that is
 * busy: the free waits for the request, whichever worker it runs on.
 */
static void LSV_KTEST(ht_free_waits_for_queued_request)(struct kunit *test)
{
	struct lsv_ht_test_job *freer = lsv_ht_test_job_new(test);
	struct lsv_ht_test_job *hold = lsv_ht_test_job_new(test);
	struct lsv_ht_test_ctx *ctx = test->priv;

	/* Arrange */
	ht_test_request_behind(ctx, hold, ht_test_hold_fn);

	/* Act */
	lsv_ht_test_job_start(ctx, freer, 0, 0, ht_test_free_fn);

	/* Assert: the free waits for the request, and is over once it runs */
	KUNIT_EXPECT_FALSE(test, wait_for_completion_timeout(&freer->done,
							     msecs_to_jiffies(100)));
	complete_all(&ctx->hold_release);
	KUNIT_ASSERT_TRUE(test, lsv_ht_test_job_wait(freer));
	KUNIT_ASSERT_TRUE(test, lsv_ht_test_job_wait(hold));
}

/*
 * The table is freed on the very worker a reclaim request is queued on, which
 * runs the request only once the free has returned.
 */
static void LSV_KTEST(ht_free_on_worker_with_queued_request)(struct kunit *test)
{
	struct lsv_ht_test_job *hold = lsv_ht_test_job_new(test);
	struct lsv_ht_test_ctx *ctx = test->priv;
	struct lsv_ht_test_job *flush;
	unsigned int i;

	/* Arrange */
	ht_test_request_behind(ctx, hold, ht_test_hold_then_free_fn);

	/* Act */
	complete_all(&ctx->hold_release);

	/* Assert: the free returns, and the request runs after it */
	KUNIT_ASSERT_TRUE(test, lsv_ht_test_job_wait(hold));
	/* a batch the worker has taken runs newest first: the second one comes last */
	for (i = 0; i < 2; i++) {
		flush = lsv_ht_test_job_new(test);
		lsv_ht_test_job_start(ctx, flush, 1, 0, ht_test_hold_fn);
		KUNIT_ASSERT_TRUE(test, lsv_ht_test_job_wait(flush));
	}
}

/*
 * What .exit does first, after a test that failed an assertion with a job
 * parked inside the table and another one holding its worker.
 */
static void LSV_KTEST(ht_join_releases_parked_jobs)(struct kunit *test)
{
	struct lsv_ht_test_job *parked = lsv_ht_test_job_new(test);
	struct lsv_ht_test_job *hold = lsv_ht_test_job_new(test);
	struct lsv_ht_test_ctx *ctx = test->priv;
	bool is_new;

	/* Arrange */
	KUNIT_ASSERT_GE(test, ctx->worker_cnt, 2U);
	KUNIT_ASSERT_EQ(test, lsv_ht_test_create_table(ctx, 64, 64), 0);
	KUNIT_ASSERT_NOT_NULL(test, lsv_ht_test_insert(ctx, 1, &is_new));
	lsv_ht_test_park_lookup(ctx, parked, 1, 1);
	lsv_ht_test_job_start(ctx, hold, 1, 0, ht_test_hold_fn);
	KUNIT_ASSERT_TRUE(test, wait_for_completion_timeout(&ctx->hold_parked,
							    msecs_to_jiffies(LSV_HT_TEST_WAIT_MS)));

	/* Act, Assert */
	KUNIT_EXPECT_TRUE(test, lsv_ht_test_join(ctx));
	KUNIT_EXPECT_EQ(test, atomic_read(&ctx->jobs_running), 0);
	KUNIT_EXPECT_TRUE(test, lsv_ht_test_release_key(ctx, 1));
}

static struct kunit_case __cases[] = {
	LSV_KUNIT_CASE(ht_insert_find_remove),
	LSV_KUNIT_CASE(ht_capacity),
	LSV_KUNIT_CASE(ht_cell_exhaustion),
	LSV_KUNIT_CASE(ht_dead_entry_reuse),
	LSV_KUNIT_CASE(ht_late_remove_spares_new_value),
	LSV_KUNIT_CASE(ht_lost_insertion_drops_its_value),
	LSV_KUNIT_CASE(ht_cell_shortage_reclaims),
	LSV_KUNIT_CASE(ht_values_without_refs),
	LSV_KUNIT_CASE(ht_late_worker_refused),
	LSV_KUNIT_CASE(ht_capacity_under_contention),
	LSV_KUNIT_CASE(ht_concurrent_participants),
	LSV_KUNIT_CASE(ht_concurrent_participants_reused_cells),
	LSV_KUNIT_CASE(ht_idle_buffer_comes_back),
	LSV_KUNIT_CASE(ht_free_waits_for_queued_request),
	LSV_KUNIT_CASE(ht_free_on_worker_with_queued_request),
	LSV_KUNIT_CASE(ht_join_releases_parked_jobs),
	{}
};

static struct kunit_suite __suite = {
	.name		= "lsv_ht_" LSV_HT_IMPL_NAME,
	.init		= lsv_ht_test_init,
	.exit		= lsv_ht_test_exit,
	.test_cases	= __cases,
};

kunit_test_suites(&__suite);
