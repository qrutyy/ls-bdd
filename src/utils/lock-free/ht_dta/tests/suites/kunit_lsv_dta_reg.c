#include <kunit/static_stub.h>
#include <kunit/test.h>
#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/hash.h>
#include <linux/jiffies.h>

#include "utils/kunit.h"
#include "utils/lock-free/ht_tests/lsv_kunit_ht.h"

/*
 * The cases of the recovery of [DtA] itself: anchors, suspects, frozen ranges
 * and the cells a recovery copies. The contract every table owes is tested in
 * ht_tests/suites.
 */

#define LSV_HT_TEST_ABSENT_KEY	0x200000

static void ht_test_insert_node_fn(struct lsv_ht_test_job *job)
{
	job->value = lsv_ht_insert_node(job->ctx->ht, job->key, job->node);
}

/*
 * The restore path links a cell its caller made: the cell itself comes back
 * when the key was absent, and the cell of the key already there otherwise,
 * with the caller's cell left untouched for the caller to give back.
 */
static void LSV_KTEST(ht_restore_links_callers_cell)(struct kunit *test)
{
	struct lsv_ht_test_job *job = lsv_ht_test_job_new(test);
	struct lsv_ht_test_ctx *ctx = test->priv;
	struct lsv_ht_test_value *dup_value;
	struct lsv_lf_list_node *node;
	struct lsv_lf_list_node *dup;

	KUNIT_ASSERT_EQ(test, lsv_ht_test_create_table(ctx, 64, 64), 0);

	/* cells as a restore makes them: from the ops, outside any operation */
	node = lsv_ht_test_node_alloc(9, ctx);
	dup = lsv_ht_test_node_alloc(9, ctx);
	KUNIT_ASSERT_NOT_NULL(test, node);
	KUNIT_ASSERT_NOT_NULL(test, dup);
	dup_value = dup->value;

	job->key = 9;
	job->node = node;
	lsv_ht_test_run(ctx, job, ht_test_insert_node_fn);
	KUNIT_EXPECT_PTR_EQ(test, job->value, (void *)node);
	KUNIT_EXPECT_EQ(test, lsv_ht_entry_cnt(ctx->ht), 1ULL);
	KUNIT_EXPECT_PTR_EQ(test, lsv_ht_test_find(ctx, 9), node->value);

	/* the same key again: the cell already there comes back */
	job = lsv_ht_test_job_new(test);
	job->key = 9;
	job->node = dup;
	lsv_ht_test_run(ctx, job, ht_test_insert_node_fn);
	KUNIT_EXPECT_PTR_EQ(test, job->value, (void *)node);
	KUNIT_EXPECT_EQ(test, lsv_ht_entry_cnt(ctx->ht), 1ULL);

	/* a reserved key is refused before the cell is looked at */
	job = lsv_ht_test_job_new(test);
	job->key = LSV_HT_KEY_HEAD_SENTINEL;
	job->node = dup;
	lsv_ht_test_run(ctx, job, ht_test_insert_node_fn);
	KUNIT_EXPECT_NULL(test, job->value);
	KUNIT_EXPECT_EQ(test, lsv_ht_entry_cnt(ctx->ht), 1ULL);

	/* the caller gives its unused cell back, with the value it made */
	KUNIT_EXPECT_TRUE(test, refcount_dec_and_test(&dup_value->ref));
	lsv_ht_test_node_dealloc(dup, ctx);

	KUNIT_EXPECT_TRUE(test, lsv_ht_test_release_key(ctx, 9));
	KUNIT_EXPECT_EQ(test, lsv_ht_entry_cnt(ctx->ht), 0ULL);
}

/*
 * Only the stalled job sees the stub; it parks once, right after dropping a
 * mid-list anchor.
 */
static void ht_test_anchor_dropped_stub(struct lsv_lf_list_node *anchor)
{
	struct kunit *test = kunit_get_current_test();
	struct lsv_ht_test_ctx *ctx;

	if (!test)
		return;
	ctx = test->priv;
	if (!atomic_xchg(&ctx->park_armed, 0))
		return;

	complete(&ctx->parked);
	wait_for_completion(&ctx->release);
}

#define LSV_HT_TEST_BUCKET_KEYS		16
#define LSV_HT_TEST_CHURN_KEY		0x100000
#define LSV_HT_TEST_CHURN_MAX_MS	10000
#define LSV_HT_TEST_RECLAIM_OPS		64
#define LSV_HT_TEST_HEAD_KEYS		3

/*
 * Keeps one key busy until the stalled participant has been recovered. A
 * recovery answers a participant that has been holding its timestamp for a
 * while, so this takes a moment; the operations in between are what notices.
 */
static void ht_test_churn_fn(struct lsv_ht_test_job *job)
{
	unsigned int ms = job->deadline_ms ? job->deadline_ms : LSV_HT_TEST_CHURN_MAX_MS;
	unsigned long deadline = jiffies + msecs_to_jiffies(ms);
	struct lsv_ht_test_ctx *ctx = job->ctx;
	struct lsv_ht_test_value *value;
	bool is_new;

	while (!lsv_ht_recovery_cnt(ctx->ht)) {
		if (time_after(jiffies, deadline))
			break;

		job->iterations++;
		value = lsv_ht_insert_or_inc_ref(ctx->ht, job->key, &is_new);
		if (!value)
			continue;
		if (refcount_dec_and_test(&value->ref))
			lsv_ht_remove(ctx->ht, job->key);
	}
}

/* Churns its key until ctx->churn_until() holds; result says whether it did. */
static void ht_test_churn_until_fn(struct lsv_ht_test_job *job)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(LSV_HT_TEST_CHURN_MAX_MS);
	struct lsv_ht_test_ctx *ctx = job->ctx;
	struct lsv_ht_test_value *value;
	bool is_new;

	while (!ctx->churn_until(ctx) && time_before(jiffies, deadline)) {
		job->iterations++;
		value = lsv_ht_insert_or_inc_ref(ctx->ht, job->key, &is_new);
		if (value && refcount_dec_and_test(&value->ref))
			lsv_ht_remove(ctx->ht, job->key);
	}
	job->result = ctx->churn_until(ctx);
}

/* Churns LSV_HT_TEST_CHURN_KEY on worker 0 of wq 1 until |until| holds. */
static void ht_test_churn_until(struct lsv_ht_test_ctx *ctx,
				bool (*until)(struct lsv_ht_test_ctx *ctx))
{
	struct lsv_ht_test_job *churn = lsv_ht_test_job_new(ctx->test);

	ctx->churn_until = until;
	churn->key = LSV_HT_TEST_CHURN_KEY;
	lsv_ht_test_job_start(ctx, churn, 1, 0, ht_test_churn_until_fn);
	KUNIT_ASSERT_TRUE(ctx->test, lsv_ht_test_job_wait(churn));
	KUNIT_ASSERT_TRUE(ctx->test, churn->result);
}

/*
 * Waits for |want| cells of the pool to come back. A churn retires a cell per
 * turn and nothing can be reclaimed while the stalled participant holds its
 * timestamp, so by the time it is recovered the pool is dry. Reclamation
 * resumes a batch per operation: a test that inserts after a recovery has to
 * let the pool refill first, or it fails on the pool instead of on what it
 * came to check.
 */
static void ht_test_wait_free_cells(struct lsv_ht_test_ctx *ctx, u64 key, int want)
{
	unsigned int n;

	for (n = 0; n < LSV_HT_TEST_RECLAIM_OPS; n++) {
		if (atomic_read(&ctx->nodes_live) + want <= ctx->nodes_max)
			return;
		lsv_ht_test_find(ctx, key);
	}
}

/*
 * An anchor names a cell of a bucket, and only the operation that set it keeps
 * that cell out of reach of reclamation. Whatever the traversal left behind,
 * the record must carry no anchor once the operation is over.
 */
static void LSV_KTEST(ht_anchor_ends_with_the_op)(struct kunit *test)
{
	struct lsv_ht_test_ctx *ctx = test->priv;
	unsigned int n = 0;
	u64 keys[LSV_HT_TEST_BUCKET_KEYS];
	unsigned int bucket;
	unsigned int tid;
	bool is_new;
	u64 key;

	KUNIT_ASSERT_EQ(test, lsv_ht_test_create_table_reserve(ctx, 1024, 1024,
							       LSV_HT_TEST_RECOVERY_RESERVE), 0);

	/* one bucket, long enough that a lookup of the last key drops anchors */
	bucket = hash_64(1, ctx->ht->bucket_bits);
	for (key = 1; n < LSV_HT_TEST_BUCKET_KEYS; key++) {
		if (hash_64(key, ctx->ht->bucket_bits) == bucket)
			keys[n++] = key;
	}
	for (n = 0; n < LSV_HT_TEST_BUCKET_KEYS; n++)
		KUNIT_ASSERT_NOT_NULL(test, lsv_ht_test_insert(ctx, keys[n], &is_new));

	tid = lsv_worker_id(lsv_ht_test_worker(ctx, 0, 0));

	KUNIT_EXPECT_NOT_NULL(test, lsv_ht_test_find(ctx, keys[LSV_HT_TEST_BUCKET_KEYS - 1]));
	KUNIT_EXPECT_FALSE(test, ht_kunit_participant_holds_anchor(ctx->ht, tid));

	KUNIT_EXPECT_TRUE(test, lsv_ht_test_release_key(ctx, keys[LSV_HT_TEST_BUCKET_KEYS - 1]));
	KUNIT_EXPECT_FALSE(test, ht_kunit_participant_holds_anchor(ctx->ht, tid));

	KUNIT_EXPECT_NOT_NULL(test, lsv_ht_test_insert(ctx, keys[0], &is_new));
	KUNIT_EXPECT_FALSE(test, ht_kunit_participant_holds_anchor(ctx->ht, tid));
}

/*
 * A participant stalled inside a traversal blocks reclamation of every cell
 * retired after it started. The others must recover it, reclaim their cells
 * meanwhile, and the stalled lookup must still finish with the right value.
 */
static void ht_test_stalled_recovery(struct kunit *test, u64 size, int cells,
				     int recovery_cells)
{
	struct lsv_ht_test_job *stalled = lsv_ht_test_job_new(test);
	struct lsv_ht_test_job *churn = lsv_ht_test_job_new(test);
	struct lsv_ht_test_ctx *ctx = test->priv;
	unsigned int n = 0;
	u64 keys[LSV_HT_TEST_BUCKET_KEYS];
	struct lsv_ht_test_value *value;
	unsigned int bucket;
	bool is_new;
	int freed;
	u64 key;

	KUNIT_ASSERT_GE(test, ctx->worker_cnt, 2U);
	KUNIT_ASSERT_EQ(test, lsv_ht_test_create_table_reserve(ctx, size, cells,
							       recovery_cells), 0);

	/* Keys of one bucket, so a lookup of the last one crosses several anchors. */
	bucket = hash_64(1, ctx->ht->bucket_bits);
	for (key = 1; n < LSV_HT_TEST_BUCKET_KEYS; key++) {
		if (hash_64(key, ctx->ht->bucket_bits) == bucket)
			keys[n++] = key;
	}
	for (n = 0; n < LSV_HT_TEST_BUCKET_KEYS; n++)
		KUNIT_ASSERT_NOT_NULL(test, lsv_ht_test_insert(ctx, keys[n], &is_new));

	value = lsv_ht_test_find(ctx, keys[LSV_HT_TEST_BUCKET_KEYS - 1]);
	KUNIT_ASSERT_NOT_NULL(test, value);

	/* Stall worker 1 of the first wq inside that lookup. */
	init_completion(&ctx->parked);
	init_completion(&ctx->release);
	atomic_set(&ctx->park_armed, 1);
	kunit_activate_static_stub(test, ht_kunit_anchor_dropped, ht_test_anchor_dropped_stub);
	stalled->key = keys[LSV_HT_TEST_BUCKET_KEYS - 1];
	stalled->stub_visible = true;
	lsv_ht_test_job_start(ctx, stalled, 0, 1, lsv_ht_test_find_fn);
	KUNIT_ASSERT_TRUE(test, wait_for_completion_timeout(&ctx->parked,
							    msecs_to_jiffies(LSV_HT_TEST_WAIT_MS)));

	/* Churn another key on worker 0: the others must recover the stalled one. */
	freed = atomic_read(&ctx->nodes_freed);
	churn->key = LSV_HT_TEST_CHURN_KEY;
	lsv_ht_test_job_start(ctx, churn, 0, 0, ht_test_churn_fn);
	KUNIT_ASSERT_TRUE(test, lsv_ht_test_job_wait(churn));

	kunit_info(test, "churn ops: %u, recoveries: %llu\n", churn->iterations,
		   lsv_ht_recovery_cnt(ctx->ht));
	KUNIT_EXPECT_GT(test, lsv_ht_recovery_cnt(ctx->ht), 0ULL);

	complete(&ctx->release);
	KUNIT_ASSERT_TRUE(test, lsv_ht_test_job_wait(stalled));
	kunit_deactivate_static_stub(test, ht_kunit_anchor_dropped);

	KUNIT_EXPECT_PTR_EQ(test, stalled->value, (void *)value);
	for (n = 0; n < LSV_HT_TEST_BUCKET_KEYS; n++) {
		value = lsv_ht_test_find(ctx, keys[n]);
		KUNIT_EXPECT_NOT_NULL(test, value);
		if (value)
			KUNIT_EXPECT_EQ(test, value->key, keys[n]);
	}

	/* the recovered range is replaced by unfrozen copies, not left in place */
	KUNIT_EXPECT_TRUE(test, lsv_ht_test_release_key(ctx, keys[LSV_HT_TEST_BUCKET_KEYS - 1]));

	/*
	 * The recovered participant has resumed, so the cells held back while it
	 * was not running, the ones a recovery froze among them, come back.
	 */
	for (n = 0; n < LSV_HT_TEST_RECLAIM_OPS; n++) {
		if (atomic_read(&ctx->nodes_freed) != freed)
			break;
		lsv_ht_test_find(ctx, keys[0]);
	}
	kunit_info(test, "freed after recovery: %d\n",
		   atomic_read(&ctx->nodes_freed) - freed);
	KUNIT_EXPECT_GT(test, atomic_read(&ctx->nodes_freed), freed);
}

static void LSV_KTEST(ht_stalled_participant)(struct kunit *test)
{
	ht_test_stalled_recovery(test, 1024, 1024, LSV_HT_TEST_RECOVERY_RESERVE);
}

/*
 * The same with a pool that holds nothing but the entries: freezing a range
 * cannot be undone, so a recovery that cannot copy its live cells would leave
 * a range no operation can remove from, in a table where removals are the only
 * source of free cells. What keeps the recovery going is that node_copy() is
 * served from cells ordinary inserts never reach.
 */
static void LSV_KTEST(ht_stalled_participant_full_pool)(struct kunit *test)
{
	/* the bucket keys and one cell for the churned key: nothing to spare */
	ht_test_stalled_recovery(test, LSV_HT_TEST_BUCKET_KEYS + 4,
				 LSV_HT_TEST_BUCKET_KEYS + 1, LSV_DTA_SPARE_CELLS);
}

/*
 * The head sentinel is the first cell a lookup counts, so its first anchor
 * lands on keys[LSV_DTA_ANCHOR_THRESHOLD - 2], and the next one would land
 * LSV_DTA_ANCHOR_THRESHOLD live cells further. The cell of that next anchor is
 * the farthest one the traversal reaches from the first: it reads the cell's
 * next pointer before it publishes the anchor.
 */
#define LSV_HT_TEST_EDGE_IDX	(2 * LSV_DTA_ANCHOR_THRESHOLD - 2)

static_assert(LSV_HT_TEST_EDGE_IDX < LSV_HT_TEST_BUCKET_KEYS);

/*
 * Has the others recover the parked lookup, remove the key of the cell it
 * stands on and reclaim around it; the table must not give that cell back
 * before the lookup has resumed. |key| is one to reclaim with.
 */
static void ht_test_reclaim_under_parked(struct lsv_ht_test_ctx *ctx,
					 struct lsv_ht_test_job *stalled, u64 key)
{
	struct kunit *test = ctx->test;
	struct lsv_ht_test_job *churn = lsv_ht_test_job_new(test);
	unsigned int n;
	int freed;

	churn->key = LSV_HT_TEST_CHURN_KEY;
	lsv_ht_test_job_start(ctx, churn, 0, 0, ht_test_churn_fn);
	KUNIT_ASSERT_TRUE(test, lsv_ht_test_job_wait(churn));
	KUNIT_ASSERT_GT(test, lsv_ht_recovery_cnt(ctx->ht), 0ULL);

	KUNIT_EXPECT_TRUE(test, lsv_ht_test_release_key(ctx, ctx->park_key));

	/* the churn left the pool dry: every cell of it is in the queue first */
	freed = atomic_read(&ctx->nodes_freed);
	for (n = 0; n < 4 * LSV_HT_TEST_RECLAIM_OPS && !READ_ONCE(ctx->held_cell_freed); n++)
		lsv_ht_test_find(ctx, key);
	kunit_info(test, "freed while parked: %d\n", atomic_read(&ctx->nodes_freed) - freed);

	/* reclamation ran, and the parked participant's cell survived it */
	KUNIT_EXPECT_GT(test, atomic_read(&ctx->nodes_freed), freed);
	KUNIT_EXPECT_FALSE(test, READ_ONCE(ctx->held_cell_freed));

	complete(&ctx->release);
	KUNIT_ASSERT_TRUE(test, lsv_ht_test_job_wait(stalled));
	kunit_deactivate_static_stub(test, ht_kunit_cell_reached);
	KUNIT_EXPECT_NOT_NULL(test, stalled->value);
}

/*
 * A recovery must freeze every cell its participant may still read. Here the
 * participant is parked on the farthest cell its anchor lets it reach.
 */
static void LSV_KTEST(ht_recovery_covers_the_anchor_window)(struct kunit *test)
{
	struct lsv_ht_test_job *stalled = lsv_ht_test_job_new(test);
	struct lsv_ht_test_ctx *ctx = test->priv;
	u64 keys[LSV_HT_TEST_BUCKET_KEYS];
	unsigned int n;
	bool is_new;

	KUNIT_ASSERT_GE(test, ctx->worker_cnt, 2U);
	KUNIT_ASSERT_EQ(test, lsv_ht_test_create_table_reserve(ctx, 1024, 1024,
							       LSV_HT_TEST_RECOVERY_RESERVE), 0);

	/* Arrange: one bucket of live cells, all older than the stalled lookup. */
	lsv_ht_test_bucket_keys(ctx, keys, LSV_HT_TEST_BUCKET_KEYS);
	for (n = 0; n < LSV_HT_TEST_BUCKET_KEYS; n++)
		KUNIT_ASSERT_NOT_NULL(test, lsv_ht_test_insert(ctx, keys[n], &is_new));

	/* Act: park a lookup on the farthest cell of its first anchor's window. */
	lsv_ht_test_park_lookup(ctx, stalled, keys[LSV_HT_TEST_EDGE_IDX],
				keys[LSV_HT_TEST_BUCKET_KEYS - 1]);

	/* Assert */
	ht_test_reclaim_under_parked(ctx, stalled, keys[0]);
}

/* the late key goes right behind the first cell after the lookup's anchor */
#define LSV_HT_TEST_LATE_IDX	LSV_DTA_ANCHOR_THRESHOLD

/*
 * An insertion stalls right before it links its cell. A lookup that begins
 * meanwhile walks past the spot, and the cell is linked behind it: the lookup
 * never counted it towards its next anchor, so a recovery that took the cell
 * for one older than the lookup would freeze a range one cell short of where
 * the lookup is parked.
 */
static void LSV_KTEST(ht_recovery_ignores_cells_linked_behind)(struct kunit *test)
{
	struct lsv_ht_test_job *inserter = lsv_ht_test_job_new(test);
	struct lsv_ht_test_job *stalled = lsv_ht_test_job_new(test);
	struct lsv_ht_test_ctx *ctx = test->priv;
	unsigned int n = 0;
	u64 all[LSV_HT_TEST_BUCKET_KEYS + 1];
	u64 keys[LSV_HT_TEST_BUCKET_KEYS];
	unsigned int i;
	bool is_new;

	KUNIT_ASSERT_GE(test, ctx->worker_cnt, 2U);
	KUNIT_ASSERT_EQ(test, lsv_ht_test_create_table_reserve(ctx, 1024, 1024,
							       LSV_HT_TEST_RECOVERY_RESERVE), 0);

	/* Arrange: one bucket of live cells, with a gap for the late key. */
	lsv_ht_test_bucket_keys(ctx, all, ARRAY_SIZE(all));
	for (i = 0; i < ARRAY_SIZE(all); i++) {
		if (i != LSV_HT_TEST_LATE_IDX)
			keys[n++] = all[i];
	}
	for (n = 0; n < LSV_HT_TEST_BUCKET_KEYS; n++)
		KUNIT_ASSERT_NOT_NULL(test, lsv_ht_test_insert(ctx, keys[n], &is_new));

	/* The insertion of the late key parks right before it links its cell. */
	init_completion(&ctx->link_parked);
	init_completion(&ctx->link_release);
	atomic_set(&ctx->link_armed, 1);
	kunit_activate_static_stub(test, ht_kunit_cell_linking, lsv_ht_test_cell_linking_stub);
	inserter->key = all[LSV_HT_TEST_LATE_IDX];
	inserter->stub_visible = true;
	lsv_ht_test_job_start(ctx, inserter, 1, 0, lsv_ht_test_insert_fn);
	KUNIT_ASSERT_TRUE(test, wait_for_completion_timeout(&ctx->link_parked,
							    msecs_to_jiffies(LSV_HT_TEST_WAIT_MS)));

	/* Two operations begin in between, so the lookup is younger than the insertion. */
	lsv_ht_test_find(ctx, keys[0]);
	lsv_ht_test_find(ctx, keys[0]);

	/* Act: a lookup walks past the gap and parks; the cell is linked behind it. */
	lsv_ht_test_park_lookup(ctx, stalled, keys[LSV_HT_TEST_EDGE_IDX],
				keys[LSV_HT_TEST_BUCKET_KEYS - 1]);
	complete(&ctx->link_release);
	KUNIT_ASSERT_TRUE(test, lsv_ht_test_job_wait(inserter));
	kunit_deactivate_static_stub(test, ht_kunit_cell_linking);
	KUNIT_ASSERT_TRUE(test, inserter->result);

	/* Assert */
	ht_test_reclaim_under_parked(ctx, stalled, keys[0]);
}

/*
 * Parks a reclamation once, between the flags and the second read of the
 * timestamp of the record of |record_tid|.
 */
static void ht_test_record_flags_read_stub(unsigned int tid)
{
	struct kunit *test = kunit_get_current_test();
	struct lsv_ht_test_ctx *ctx;

	if (!test)
		return;
	ctx = test->priv;
	if (tid != ctx->record_tid || !atomic_xchg(&ctx->record_armed, 0))
		return;

	complete(&ctx->record_parked);
	wait_for_completion(&ctx->record_release);
}

/*
 * Reclamation reads the record of every participant while any of them may be
 * recovered under it. A reader that paired the flags of the lookup still
 * running with the timestamp the recovery gave it would count the lookup as a
 * running one that began after the recovery -- and free the cell it has been
 * standing on since before.
 */
static void LSV_KTEST(ht_reclaim_reads_one_record_state)(struct kunit *test)
{
	struct lsv_ht_test_job *reclaimer = lsv_ht_test_job_new(test);
	struct lsv_ht_test_job *stalled = lsv_ht_test_job_new(test);
	struct lsv_ht_test_job *churn = lsv_ht_test_job_new(test);
	struct lsv_ht_test_ctx *ctx = test->priv;
	u64 keys[LSV_HT_TEST_BUCKET_KEYS];
	unsigned int n;
	bool is_new;

	KUNIT_ASSERT_GE(test, ctx->worker_cnt, 2U);
	/*
	 * Past LSV_HT_DEF_FILL_THRESHOLD whatever the churn does, so that every
	 * operation reclaims, the one parked below included.
	 */
	KUNIT_ASSERT_EQ(test, lsv_ht_test_create_table_reserve(ctx, LSV_HT_TEST_BUCKET_KEYS + 2,
							       1024,
							       LSV_HT_TEST_RECOVERY_RESERVE), 0);

	lsv_ht_test_bucket_keys(ctx, keys, LSV_HT_TEST_BUCKET_KEYS);
	KUNIT_ASSERT_NE(test, hash_64(LSV_HT_TEST_CHURN_KEY, ctx->ht->bucket_bits),
			hash_64(keys[0], ctx->ht->bucket_bits));
	for (n = 0; n < LSV_HT_TEST_BUCKET_KEYS; n++)
		KUNIT_ASSERT_NOT_NULL(test, lsv_ht_test_insert(ctx, keys[n], &is_new));

	/*
	 * Arrange: a lookup parked on a cell, and the cell removed under it and
	 * retired by worker 0, which is going to reclaim it.
	 */
	lsv_ht_test_park_lookup(ctx, stalled, keys[LSV_HT_TEST_EDGE_IDX],
				keys[LSV_HT_TEST_BUCKET_KEYS - 1]);
	KUNIT_ASSERT_TRUE(test, lsv_ht_test_release_key(ctx, ctx->park_key));
	/* two operations in between, so the reclaiming one is younger than the cell */
	lsv_ht_test_find(ctx, keys[0]);
	lsv_ht_test_find(ctx, keys[0]);

	/* Act: worker 0 reclaims and stops halfway through the lookup's record... */
	init_completion(&ctx->record_parked);
	init_completion(&ctx->record_release);
	ctx->record_tid = lsv_worker_id(lsv_ht_test_worker(ctx, 0, 1));
	atomic_set(&ctx->record_armed, 1);
	kunit_activate_static_stub(test, ht_kunit_record_flags_read,
				   ht_test_record_flags_read_stub);
	reclaimer->key = keys[0];
	reclaimer->stub_visible = true;
	lsv_ht_test_job_start(ctx, reclaimer, 0, 0, lsv_ht_test_find_fn);
	KUNIT_ASSERT_TRUE(test, wait_for_completion_timeout(&ctx->record_parked,
							    msecs_to_jiffies(LSV_HT_TEST_WAIT_MS)));

	/* ...while another worker suspects the lookup and recovers it */
	churn->key = LSV_HT_TEST_CHURN_KEY;
	lsv_ht_test_job_start(ctx, churn, 1, 0, ht_test_churn_fn);
	KUNIT_ASSERT_TRUE(test, lsv_ht_test_job_wait(churn));
	KUNIT_ASSERT_GT(test, lsv_ht_recovery_cnt(ctx->ht), 0ULL);

	complete(&ctx->record_release);
	KUNIT_ASSERT_TRUE(test, lsv_ht_test_job_wait(reclaimer));
	kunit_deactivate_static_stub(test, ht_kunit_record_flags_read);

	/* Assert: the cell the lookup stands on survived the reclamation */
	KUNIT_EXPECT_FALSE(test, READ_ONCE(ctx->held_cell_freed));

	complete(&ctx->release);
	KUNIT_ASSERT_TRUE(test, lsv_ht_test_job_wait(stalled));
	kunit_deactivate_static_stub(test, ht_kunit_cell_reached);
	KUNIT_EXPECT_NOT_NULL(test, stalled->value);
}

/* Parks the operation of |begin_tid| once, its timestamp computed and not published. */
static void ht_test_begin_computed_stub(unsigned int tid)
{
	struct kunit *test = kunit_get_current_test();
	struct lsv_ht_test_ctx *ctx;

	if (!test)
		return;
	ctx = test->priv;
	if (tid != ctx->begin_tid || !atomic_xchg(&ctx->begin_armed, 0))
		return;

	complete(&ctx->begin_parked);
	wait_for_completion(&ctx->begin_release);
}

static bool ht_test_watched_suspected(struct lsv_ht_test_ctx *ctx)
{
	return ht_kunit_participant_is_suspected(ctx->ht, ctx->watch_tid);
}

static bool ht_test_watched_gave_up(struct lsv_ht_test_ctx *ctx)
{
	return ht_kunit_participant_is_suspected(ctx->ht, ctx->watch_tid) &&
	       ht_kunit_recovery_aborts(ctx->ht);
}

static bool ht_test_two_recovered(struct lsv_ht_test_ctx *ctx)
{
	return lsv_ht_recovery_cnt(ctx->ht) >= 2;
}

/* the first anchor of a lookup in a bucket of LSV_HT_TEST_BUCKET_KEYS, and past its window */
#define LSV_HT_TEST_YOUNGER_PARK	LSV_DTA_ANCHOR_THRESHOLD
#define LSV_HT_TEST_COPY_BLOCK_MAX	(2 * LSV_DTA_ANCHOR_THRESHOLD - 3)
#define LSV_HT_TEST_OLDER_PARK		(2 * LSV_DTA_ANCHOR_THRESHOLD + 1)

static_assert(LSV_HT_TEST_OLDER_PARK < LSV_HT_TEST_BUCKET_KEYS);

/*
 * A timestamp is published by a CAS of its own record only, so one computed
 * before a stall may come out below that of a participant that began later
 * and has been suspected since: two participants are suspected at once.
 *
 * Here the younger lookup is suspected first, and its recovery freezes its
 * range but cannot copy it. The older one then publishes its stale timestamp,
 * stalls past that range and is suspected in turn, and its recovery runs into
 * the range frozen for the younger one and gives up. Neither resumes, and no
 * operation comes near their bucket: the table has to help the younger one
 * for either recovery to finish.
 */
static void LSV_KTEST(ht_stale_begin_second_suspect)(struct kunit *test)
{
	struct lsv_ht_test_job *younger = lsv_ht_test_job_new(test);
	struct lsv_ht_test_job *older = lsv_ht_test_job_new(test);
	struct lsv_ht_test_ctx *ctx = test->priv;
	u64 keys[LSV_HT_TEST_BUCKET_KEYS];
	struct lsv_ht_test_value *value;
	unsigned int younger_tid;
	unsigned int n;
	bool is_new;

	KUNIT_ASSERT_GE(test, ctx->worker_cnt, 2U);
	KUNIT_ASSERT_EQ(test, lsv_ht_test_create_table_reserve(ctx, 1024, 1024,
							       LSV_HT_TEST_RECOVERY_RESERVE), 0);

	lsv_ht_test_bucket_keys(ctx, keys, LSV_HT_TEST_BUCKET_KEYS);
	KUNIT_ASSERT_NE(test, hash_64(LSV_HT_TEST_CHURN_KEY, ctx->ht->bucket_bits),
			hash_64(keys[0], ctx->ht->bucket_bits));
	for (n = 0; n < LSV_HT_TEST_BUCKET_KEYS; n++)
		KUNIT_ASSERT_NOT_NULL(test, lsv_ht_test_insert(ctx, keys[n], &is_new));
	value = lsv_ht_test_find(ctx, keys[LSV_HT_TEST_BUCKET_KEYS - 1]);
	KUNIT_ASSERT_NOT_NULL(test, value);

	kunit_activate_static_stub(test, ht_kunit_begin_computed, ht_test_begin_computed_stub);
	kunit_activate_static_stub(test, ht_kunit_cell_reached, lsv_ht_test_cell_reached_stub);

	/* Arrange: the older lookup computes its timestamp and stalls before publishing it */
	ctx->begin_tid = lsv_worker_id(lsv_ht_test_worker(ctx, 0, 1));
	atomic_set(&ctx->begin_armed, 1);
	older->key = keys[LSV_HT_TEST_BUCKET_KEYS - 1];
	older->stub_visible = true;
	lsv_ht_test_job_start(ctx, older, 0, 1, lsv_ht_test_find_fn);
	KUNIT_ASSERT_TRUE(test, wait_for_completion_timeout(&ctx->begin_parked,
							    msecs_to_jiffies(LSV_HT_TEST_WAIT_MS)));

	/* two operations publish timestamps above it, then the younger lookup begins */
	lsv_ht_test_find(ctx, keys[0]);
	lsv_ht_test_find(ctx, keys[0]);
	init_completion(&ctx->parked);
	init_completion(&ctx->release);
	ctx->park_key = keys[LSV_HT_TEST_YOUNGER_PARK];
	atomic_set(&ctx->park_armed, 1);
	younger->key = keys[LSV_HT_TEST_BUCKET_KEYS - 1];
	younger->stub_visible = true;
	lsv_ht_test_job_start(ctx, younger, 1, 1, lsv_ht_test_find_fn);
	KUNIT_ASSERT_TRUE(test, wait_for_completion_timeout(&ctx->parked,
							    msecs_to_jiffies(LSV_HT_TEST_WAIT_MS)));
	younger_tid = lsv_worker_id(lsv_ht_test_worker(ctx, 1, 1));

	/* it is suspected, and its range frozen but not copied: node_copy() refuses it */
	ctx->copy_block_max = keys[LSV_HT_TEST_COPY_BLOCK_MAX];
	atomic_set(&ctx->copy_blocked, 1);
	ctx->watch_tid = younger_tid;
	ht_test_churn_until(ctx, ht_test_watched_suspected);
	KUNIT_ASSERT_EQ(test, lsv_ht_recovery_cnt(ctx->ht), 0ULL);

	/* the older lookup publishes its stale timestamp and stalls past that range */
	ctx->park2_key = keys[LSV_HT_TEST_OLDER_PARK];
	atomic_set(&ctx->park2_armed, 1);
	complete(&ctx->begin_release);
	KUNIT_ASSERT_TRUE(test, wait_for_completion_timeout(&ctx->parked2,
							    msecs_to_jiffies(LSV_HT_TEST_WAIT_MS)));

	/* Act: it is the oldest now, is suspected in turn, and its recovery gives up */
	ctx->watch_tid = ctx->begin_tid;
	ht_test_churn_until(ctx, ht_test_watched_gave_up);
	KUNIT_EXPECT_TRUE(test, ht_kunit_participant_is_suspected(ctx->ht, younger_tid));
	KUNIT_EXPECT_EQ(test, lsv_ht_recovery_cnt(ctx->ht), 0ULL);

	/* node_copy() is served again, and neither lookup has resumed */
	atomic_set(&ctx->copy_blocked, 0);
	ht_test_churn_until(ctx, ht_test_two_recovered);

	/* Assert: both recovered while stalled, the one that gave up included */
	kunit_info(test, "recoveries=%llu passes given up=%llu\n",
		   lsv_ht_recovery_cnt(ctx->ht), ht_kunit_recovery_aborts(ctx->ht));
	KUNIT_EXPECT_GE(test, lsv_ht_recovery_cnt(ctx->ht), 2ULL);
	KUNIT_EXPECT_GT(test, ht_kunit_recovery_aborts(ctx->ht), 0ULL);

	complete(&ctx->release2);
	complete(&ctx->release);
	KUNIT_ASSERT_TRUE(test, lsv_ht_test_job_wait(older));
	KUNIT_ASSERT_TRUE(test, lsv_ht_test_job_wait(younger));
	kunit_deactivate_static_stub(test, ht_kunit_cell_reached);
	kunit_deactivate_static_stub(test, ht_kunit_begin_computed);

	/* both lookups end on the value they wanted, and every key keeps its own */
	KUNIT_EXPECT_PTR_EQ(test, older->value, (void *)value);
	KUNIT_EXPECT_PTR_EQ(test, younger->value, (void *)value);
	for (n = 0; n < LSV_HT_TEST_BUCKET_KEYS; n++) {
		value = lsv_ht_test_find(ctx, keys[n]);
		KUNIT_EXPECT_NOT_NULL(test, value);
		if (value)
			KUNIT_EXPECT_EQ(test, value->key, keys[n]);
	}
}

/* long enough for the table to suspect the stalled participant and try */
#define LSV_HT_TEST_STARVE_MS	3000

/*
 * A recovery that cannot copy must cost a pass and not the range: what a pass
 * froze stays frozen, the participant stays suspected, and a later pass picks
 * it up. Nothing in the table is reclaimed meanwhile, so the cells cannot come
 * back on their own -- here the test is what hands them over.
 */
static void LSV_KTEST(ht_recovery_waits_for_cells)(struct kunit *test)
{
	struct lsv_ht_test_job *stalled = lsv_ht_test_job_new(test);
	struct lsv_ht_test_job *churn = lsv_ht_test_job_new(test);
	struct lsv_ht_test_ctx *ctx = test->priv;
	unsigned int n = 0;
	u64 keys[LSV_HT_TEST_BUCKET_KEYS];
	struct lsv_ht_test_value *value;
	unsigned int bucket;
	unsigned int tid;
	bool is_new;
	u64 key;

	KUNIT_ASSERT_GE(test, ctx->worker_cnt, 2U);
	KUNIT_ASSERT_EQ(test, lsv_ht_test_create_table_reserve(ctx, 1024, 1024,
							       LSV_HT_TEST_RECOVERY_RESERVE), 0);

	bucket = hash_64(1, ctx->ht->bucket_bits);
	for (key = 1; n < LSV_HT_TEST_BUCKET_KEYS; key++) {
		if (hash_64(key, ctx->ht->bucket_bits) == bucket)
			keys[n++] = key;
	}
	KUNIT_ASSERT_NE(test, hash_64(LSV_HT_TEST_CHURN_KEY, ctx->ht->bucket_bits), bucket);

	for (n = 0; n < LSV_HT_TEST_BUCKET_KEYS; n++)
		KUNIT_ASSERT_NOT_NULL(test, lsv_ht_test_insert(ctx, keys[n], &is_new));

	value = lsv_ht_test_find(ctx, keys[LSV_HT_TEST_BUCKET_KEYS - 1]);
	KUNIT_ASSERT_NOT_NULL(test, value);

	/* Stall worker 1 of the first wq with nothing for node_copy() to give. */
	atomic_set(&ctx->copy_blocked, 1);
	init_completion(&ctx->parked);
	init_completion(&ctx->release);
	atomic_set(&ctx->park_armed, 1);
	kunit_activate_static_stub(test, ht_kunit_anchor_dropped, ht_test_anchor_dropped_stub);
	stalled->key = keys[LSV_HT_TEST_BUCKET_KEYS - 1];
	stalled->stub_visible = true;
	lsv_ht_test_job_start(ctx, stalled, 0, 1, lsv_ht_test_find_fn);
	KUNIT_ASSERT_TRUE(test, wait_for_completion_timeout(&ctx->parked,
							    msecs_to_jiffies(LSV_HT_TEST_WAIT_MS)));

	tid = lsv_worker_id(lsv_ht_test_worker(ctx, 0, 1));

	churn->key = LSV_HT_TEST_CHURN_KEY;
	churn->deadline_ms = LSV_HT_TEST_STARVE_MS;
	lsv_ht_test_job_start(ctx, churn, 0, 0, ht_test_churn_fn);
	KUNIT_ASSERT_TRUE(test, lsv_ht_test_job_wait(churn));

	/* suspected, and no recovery over: the range it froze is still frozen */
	KUNIT_EXPECT_TRUE(test, ht_kunit_participant_is_suspected(ctx->ht, tid));
	KUNIT_EXPECT_EQ(test, lsv_ht_recovery_cnt(ctx->ht), 0ULL);

	/* hand node_copy() the cells it was missing */
	atomic_set(&ctx->copy_blocked, 0);

	churn->deadline_ms = 0;
	lsv_ht_test_job_start(ctx, churn, 0, 0, ht_test_churn_fn);
	KUNIT_ASSERT_TRUE(test, lsv_ht_test_job_wait(churn));
	KUNIT_EXPECT_GT(test, lsv_ht_recovery_cnt(ctx->ht), 0ULL);

	complete(&ctx->release);
	KUNIT_ASSERT_TRUE(test, lsv_ht_test_job_wait(stalled));
	kunit_deactivate_static_stub(test, ht_kunit_anchor_dropped);

	/* the lookup that waited it out still ends on the value it wanted */
	KUNIT_EXPECT_PTR_EQ(test, stalled->value, (void *)value);
	for (n = 0; n < LSV_HT_TEST_BUCKET_KEYS; n++) {
		value = lsv_ht_test_find(ctx, keys[n]);
		KUNIT_EXPECT_NOT_NULL(test, value);
		if (value)
			KUNIT_EXPECT_EQ(test, value->key, keys[n]);
	}
	KUNIT_EXPECT_TRUE(test, lsv_ht_test_release_key(ctx, keys[0]));
}

/*
 * A bucket short enough that a traversal of it never drops an anchor of its
 * own leaves the participant anchored on the head sentinel, which is where
 * most operations spend their time. The range frozen for it starts at that
 * sentinel, and the copies replacing the range are spliced onto its frozen
 * next pointer.
 */
static void LSV_KTEST(ht_stalled_at_bucket_head)(struct kunit *test)
{
	struct lsv_ht_test_job *stalled = lsv_ht_test_job_new(test);
	struct lsv_ht_test_job *churn = lsv_ht_test_job_new(test);
	struct lsv_ht_test_ctx *ctx = test->priv;
	unsigned int n = 0;
	u64 keys[LSV_HT_TEST_HEAD_KEYS];
	struct lsv_ht_test_value *value;
	unsigned int bucket;
	bool is_new;
	u64 key;

	KUNIT_ASSERT_GE(test, ctx->worker_cnt, 2U);
	KUNIT_ASSERT_EQ(test, lsv_ht_test_create_table_reserve(ctx, 1024, 1024,
							       LSV_HT_TEST_RECOVERY_RESERVE), 0);

	bucket = hash_64(1, ctx->ht->bucket_bits);
	for (key = 1; n < LSV_HT_TEST_HEAD_KEYS; key++) {
		if (hash_64(key, ctx->ht->bucket_bits) == bucket)
			keys[n++] = key;
	}
	for (n = 0; n < LSV_HT_TEST_HEAD_KEYS - 1; n++)
		KUNIT_ASSERT_NOT_NULL(test, lsv_ht_test_insert(ctx, keys[n], &is_new));

	/* Stall worker 1 inside an insertion into that bucket. */
	init_completion(&ctx->parked);
	init_completion(&ctx->release);
	atomic_set(&ctx->park_in_alloc, 1);
	stalled->key = keys[LSV_HT_TEST_HEAD_KEYS - 1];
	lsv_ht_test_job_start(ctx, stalled, 0, 1, lsv_ht_test_insert_fn);
	KUNIT_ASSERT_TRUE(test, wait_for_completion_timeout(&ctx->parked,
							    msecs_to_jiffies(LSV_HT_TEST_WAIT_MS)));

	churn->key = LSV_HT_TEST_CHURN_KEY;
	lsv_ht_test_job_start(ctx, churn, 0, 0, ht_test_churn_fn);
	KUNIT_ASSERT_TRUE(test, lsv_ht_test_job_wait(churn));
	KUNIT_EXPECT_GT(test, lsv_ht_recovery_cnt(ctx->ht), 0ULL);

	complete(&ctx->release);
	KUNIT_ASSERT_TRUE(test, lsv_ht_test_job_wait(stalled));

	/* the interrupted insertion restarts and still lands */
	KUNIT_EXPECT_NOT_NULL(test, stalled->value);
	KUNIT_EXPECT_TRUE(test, stalled->result);

	for (n = 0; n < LSV_HT_TEST_HEAD_KEYS; n++) {
		value = lsv_ht_test_find(ctx, keys[n]);
		KUNIT_EXPECT_NOT_NULL(test, value);
		if (value)
			KUNIT_EXPECT_EQ(test, value->key, keys[n]);
	}
	/* the churned key may be in or out, depending on where its loop stopped */
	KUNIT_EXPECT_GE(test, lsv_ht_entry_cnt(ctx->ht), (u64)LSV_HT_TEST_HEAD_KEYS);
}

/*
 * The same on a bucket that holds nothing at all. The range frozen for a
 * participant anchored on the head of an empty bucket has no live cell in it,
 * so there is no copy to splice the head's frozen next pointer out with, and
 * a lookup refuses to unlink through a frozen predecessor: a head left frozen
 * would turn the bucket away from every later operation, this one's own
 * insertion included.
 */
static void LSV_KTEST(ht_stalled_at_empty_bucket_head)(struct kunit *test)
{
	struct lsv_ht_test_job *stalled = lsv_ht_test_job_new(test);
	struct lsv_ht_test_job *churn = lsv_ht_test_job_new(test);
	struct lsv_ht_test_ctx *ctx = test->priv;
	unsigned int n = 0;
	u64 keys[LSV_HT_TEST_HEAD_KEYS];
	struct lsv_ht_test_value *value;
	unsigned int bucket;
	bool is_new;
	u64 key;

	KUNIT_ASSERT_GE(test, ctx->worker_cnt, 2U);
	KUNIT_ASSERT_EQ(test, lsv_ht_test_create_table_reserve(ctx, 1024, 1024,
							       LSV_HT_TEST_RECOVERY_RESERVE), 0);

	bucket = hash_64(1, ctx->ht->bucket_bits);
	for (key = 1; n < LSV_HT_TEST_HEAD_KEYS; key++) {
		if (hash_64(key, ctx->ht->bucket_bits) == bucket)
			keys[n++] = key;
	}
	/* the churn has to work on a bucket of its own */
	KUNIT_ASSERT_NE(test, hash_64(LSV_HT_TEST_CHURN_KEY, ctx->ht->bucket_bits), bucket);

	/* Stall worker 1 inside an insertion into the empty bucket. */
	init_completion(&ctx->parked);
	init_completion(&ctx->release);
	atomic_set(&ctx->park_in_alloc, 1);
	stalled->key = keys[0];
	lsv_ht_test_job_start(ctx, stalled, 0, 1, lsv_ht_test_insert_fn);
	KUNIT_ASSERT_TRUE(test, wait_for_completion_timeout(&ctx->parked,
							    msecs_to_jiffies(LSV_HT_TEST_WAIT_MS)));

	churn->key = LSV_HT_TEST_CHURN_KEY;
	lsv_ht_test_job_start(ctx, churn, 0, 0, ht_test_churn_fn);
	KUNIT_ASSERT_TRUE(test, lsv_ht_test_job_wait(churn));
	KUNIT_EXPECT_GT(test, lsv_ht_recovery_cnt(ctx->ht), 0ULL);

	complete(&ctx->release);
	KUNIT_ASSERT_TRUE(test, lsv_ht_test_job_wait(stalled));

	/* the interrupted insertion restarts and still lands */
	KUNIT_EXPECT_NOT_NULL(test, stalled->value);
	KUNIT_EXPECT_TRUE(test, stalled->result);

	/* and the bucket still serves everything that comes after it */
	ht_test_wait_free_cells(ctx, keys[0], LSV_HT_TEST_HEAD_KEYS);
	kunit_info(test, "cells live after recovery: %d of %d\n",
		   atomic_read(&ctx->nodes_live), ctx->nodes_max);
	for (n = 1; n < LSV_HT_TEST_HEAD_KEYS; n++)
		KUNIT_EXPECT_NOT_NULL(test, lsv_ht_test_insert(ctx, keys[n], &is_new));
	for (n = 0; n < LSV_HT_TEST_HEAD_KEYS; n++) {
		value = lsv_ht_test_find(ctx, keys[n]);
		KUNIT_EXPECT_NOT_NULL(test, value);
		if (value)
			KUNIT_EXPECT_EQ(test, value->key, keys[n]);
	}
	KUNIT_EXPECT_TRUE(test, lsv_ht_test_release_key(ctx, keys[0]));
}

/*
 * The cells of a range wider than one recovery may take at once. A freeze
 * stops LSV_DTA_ANCHOR_THRESHOLD cells older than the stalled operation, and
 * cells inserted after it stalled are not among them: they widen the range
 * without bringing it closer to its end. Between the anchor and the last of
 * those older cells a bucket can therefore hold more live cells than
 * LSV_DTA_SPARE_LIMIT, which is all one recovery is allowed to secure.
 *
 * |LSV_HT_TEST_WIDE_HEAD| is what it takes for the traversal to drop an anchor:
 * the head sentinel is the first cell it counts, so the anchor lands on the
 * last key before the gap the filler keys go into.
 */
#define LSV_HT_TEST_WIDE_HEAD	(LSV_DTA_ANCHOR_THRESHOLD - 1)
#define LSV_HT_TEST_WIDE_FILL	(LSV_DTA_SPARE_LIMIT + 8)
#define LSV_HT_TEST_WIDE_TAIL	LSV_DTA_ANCHOR_THRESHOLD
#define LSV_HT_TEST_WIDE_KEYS	(LSV_HT_TEST_WIDE_HEAD + LSV_HT_TEST_WIDE_FILL + \
				 LSV_HT_TEST_WIDE_TAIL)
#define LSV_HT_TEST_WIDE_PACE_US	200

/*
 * Waits for the stalled participant to be recovered, on one key of its own and
 * at a pace: every turn retires a cell that no reclamation bound can free
 * while that participant holds its timestamp, and it is those turns that end
 * up suspecting it. The recovery of a wide range needs the pool, so the thread
 * waiting for it works on a single key and gives the pool back every turn.
 */
static void ht_test_wait_recovery_fn(struct lsv_ht_test_job *job)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(LSV_HT_TEST_CHURN_MAX_MS);
	struct lsv_ht_test_ctx *ctx = job->ctx;
	struct lsv_ht_test_value *value;
	bool is_new;

	while (!lsv_ht_recovery_cnt(ctx->ht)) {
		if (time_after(jiffies, deadline))
			break;

		job->iterations++;

		value = lsv_ht_insert_or_inc_ref(ctx->ht, job->key, &is_new);
		if (value && refcount_dec_and_test(&value->ref))
			lsv_ht_remove(ctx->ht, job->key);

		usleep_range(LSV_HT_TEST_WIDE_PACE_US, 2 * LSV_HT_TEST_WIDE_PACE_US);
	}
}

/*
 * A participant stalls, and the bucket it stalled in then grows behind its
 * anchor until the range a recovery has to replace is wider than the cells
 * one recovery may hold. The recovery still has to happen: nothing shortens
 * that range afterwards, so a recoverer that gives up here gives up for good,
 * and the participant it left marked stuck keeps its timestamp, which is the
 * bound every other participant reclaims against.
 */
static void ht_test_stalled_wide_range(struct kunit *test, int cells, int recovery_cells)
{
	struct lsv_ht_test_job *stalled = lsv_ht_test_job_new(test);
	struct lsv_ht_test_job *waiter = lsv_ht_test_job_new(test);
	struct lsv_ht_test_ctx *ctx = test->priv;
	unsigned int n = 0;
	struct lsv_ht_test_value *value;
	u64 keys[LSV_HT_TEST_WIDE_KEYS];
	unsigned int bucket;
	bool is_new;
	int freed;
	u64 key;

	KUNIT_ASSERT_GE(test, ctx->worker_cnt, 2U);
	KUNIT_ASSERT_EQ(test, lsv_ht_test_create_table_reserve(ctx, 4 * LSV_HT_TEST_WIDE_KEYS,
							       cells, recovery_cells), 0);

	/* Keys of one bucket, in the order the list keeps them. */
	bucket = hash_64(1, ctx->ht->bucket_bits);
	for (key = 1; n < LSV_HT_TEST_WIDE_KEYS; key++) {
		if (hash_64(key, ctx->ht->bucket_bits) == bucket)
			keys[n++] = key;
	}

	/*
	 * Everything but the filler keys: the cells the stalled traversal
	 * passes before its anchor, and the older cells that end the range
	 * frozen for it.
	 */
	for (n = 0; n < LSV_HT_TEST_WIDE_HEAD; n++)
		KUNIT_ASSERT_NOT_NULL(test, lsv_ht_test_insert(ctx, keys[n], &is_new));
	for (n = LSV_HT_TEST_WIDE_HEAD + LSV_HT_TEST_WIDE_FILL; n < LSV_HT_TEST_WIDE_KEYS; n++)
		KUNIT_ASSERT_NOT_NULL(test, lsv_ht_test_insert(ctx, keys[n], &is_new));

	value = lsv_ht_test_find(ctx, keys[LSV_HT_TEST_WIDE_KEYS - 1]);
	KUNIT_ASSERT_NOT_NULL(test, value);

	/* Stall worker 1 of the first wq on the anchor it drops in that bucket. */
	init_completion(&ctx->parked);
	init_completion(&ctx->release);
	atomic_set(&ctx->park_armed, 1);
	kunit_activate_static_stub(test, ht_kunit_anchor_dropped, ht_test_anchor_dropped_stub);
	stalled->key = keys[LSV_HT_TEST_WIDE_KEYS - 1];
	stalled->stub_visible = true;
	lsv_ht_test_job_start(ctx, stalled, 0, 1, lsv_ht_test_find_fn);
	KUNIT_ASSERT_TRUE(test, wait_for_completion_timeout(&ctx->parked,
							    msecs_to_jiffies(LSV_HT_TEST_WAIT_MS)));

	/*
	 * Widen the range behind the anchor. None of these keys is older than
	 * the stalled operation, so none of them counts towards the end of the
	 * range: it now holds more live cells than a recovery may secure.
	 */
	for (n = LSV_HT_TEST_WIDE_HEAD; n < LSV_HT_TEST_WIDE_HEAD + LSV_HT_TEST_WIDE_FILL; n++)
		KUNIT_ASSERT_NOT_NULL(test, lsv_ht_test_insert(ctx, keys[n], &is_new));
	KUNIT_ASSERT_EQ(test, lsv_ht_entry_cnt(ctx->ht), (u64)LSV_HT_TEST_WIDE_KEYS);

	freed = atomic_read(&ctx->nodes_freed);
	waiter->key = LSV_HT_TEST_CHURN_KEY;
	lsv_ht_test_job_start(ctx, waiter, 0, 0, ht_test_wait_recovery_fn);
	KUNIT_ASSERT_TRUE(test, lsv_ht_test_job_wait(waiter));

	kunit_info(test, "wait ops: %u, recoveries: %llu\n", waiter->iterations,
		   lsv_ht_recovery_cnt(ctx->ht));
	KUNIT_EXPECT_GT(test, lsv_ht_recovery_cnt(ctx->ht), 0ULL);

	complete(&ctx->release);
	KUNIT_ASSERT_TRUE(test, lsv_ht_test_job_wait(stalled));
	kunit_deactivate_static_stub(test, ht_kunit_anchor_dropped);

	/* the restarted lookup still ends on the value it was looking for */
	KUNIT_EXPECT_PTR_EQ(test, stalled->value, (void *)value);
	for (n = 0; n < LSV_HT_TEST_WIDE_KEYS; n++) {
		value = lsv_ht_test_find(ctx, keys[n]);
		KUNIT_EXPECT_NOT_NULL(test, value);
		if (value)
			KUNIT_EXPECT_EQ(test, value->key, keys[n]);
	}

	/* the whole range is replaced by unfrozen copies, not left in place */
	for (n = 0; n < LSV_HT_TEST_WIDE_KEYS; n++)
		KUNIT_EXPECT_TRUE(test, lsv_ht_test_release_key(ctx, keys[n]));

	/*
	 * The recovered participant has resumed, so the cells held back while
	 * it was not running, the whole frozen range among them, come back.
	 */
	for (n = 0; n < LSV_HT_TEST_RECLAIM_OPS; n++) {
		if (atomic_read(&ctx->nodes_freed) != freed)
			break;
		lsv_ht_test_find(ctx, keys[0]);
	}
	kunit_info(test, "freed after recovery: %d\n",
		   atomic_read(&ctx->nodes_freed) - freed);
	KUNIT_EXPECT_GT(test, atomic_read(&ctx->nodes_freed), freed);
}

static void LSV_KTEST(ht_stalled_range_outgrows_reserve)(struct kunit *test)
{
	ht_test_stalled_wide_range(test, 4 * LSV_HT_TEST_WIDE_KEYS, LSV_HT_TEST_RECOVERY_RESERVE);
}

/*
 * The same with nothing to spare: a pool that holds the entries and a cell
 * for the key the waiter works on, and no more than LSV_DTA_SPARE_CELLS kept
 * for node_copy(). The range takes more copies than that, and none of the
 * cells a pass spends comes back before the recovery is over, so what carries
 * the recovery through is the allocator node_copy() falls back on.
 */
static void LSV_KTEST(ht_stalled_range_full_pool)(struct kunit *test)
{
	struct lsv_ht_test_ctx *ctx = test->priv;

	static_assert(LSV_HT_TEST_WIDE_FILL > LSV_DTA_SPARE_CELLS);

	ctx->copy_fallback = true;
	ht_test_stalled_wide_range(test, LSV_HT_TEST_WIDE_KEYS + 1, LSV_DTA_SPARE_CELLS);

	kunit_info(test, "cells from the fallback: %d\n", atomic_read(&ctx->fallback_cnt));
	KUNIT_EXPECT_GT(test, atomic_read(&ctx->fallback_cnt), 0);
}

/*
 * The walks of the recovery of a range that takes several passes. A pass
 * copies at most LSV_DTA_SPARE_LIMIT cells, but walks all of the range left
 * frozen and the bucket up to it, so the total grows with the square of the
 * range. The bound checked is the one of that alglsvthm: a pass for every
 * LSV_DTA_SPARE_LIMIT live cells, and three walks of the bucket per pass.
 */
static void ht_test_recovery_walks(struct kunit *test, unsigned int fill)
{
	unsigned int total = LSV_HT_TEST_WIDE_HEAD + fill + LSV_HT_TEST_WIDE_TAIL;
	struct lsv_ht_test_job *stalled = lsv_ht_test_job_new(test);
	struct lsv_ht_test_job *waiter = lsv_ht_test_job_new(test);
	struct lsv_ht_test_ctx *ctx = test->priv;
	unsigned int passes;
	unsigned int live;
	unsigned int n;
	bool is_new;
	u64 walked;
	u64 *keys;
	u64 bound;

	KUNIT_ASSERT_GE(test, ctx->worker_cnt, 2U);
	keys = kunit_kcalloc(test, total, sizeof(*keys), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, keys);
	KUNIT_ASSERT_EQ(test, lsv_ht_test_create_table_reserve(ctx, 4 * total, 4 * total,
							       4 * total), 0);

	/* Arrange: a lookup stalls on its anchor, and its range then grows behind it */
	lsv_ht_test_bucket_keys(ctx, keys, total);
	for (n = 0; n < LSV_HT_TEST_WIDE_HEAD; n++)
		KUNIT_ASSERT_NOT_NULL(test, lsv_ht_test_insert(ctx, keys[n], &is_new));
	for (n = LSV_HT_TEST_WIDE_HEAD + fill; n < total; n++)
		KUNIT_ASSERT_NOT_NULL(test, lsv_ht_test_insert(ctx, keys[n], &is_new));

	init_completion(&ctx->parked);
	init_completion(&ctx->release);
	atomic_set(&ctx->park_armed, 1);
	kunit_activate_static_stub(test, ht_kunit_anchor_dropped, ht_test_anchor_dropped_stub);
	stalled->key = keys[total - 1];
	stalled->stub_visible = true;
	lsv_ht_test_job_start(ctx, stalled, 0, 1, lsv_ht_test_find_fn);
	KUNIT_ASSERT_TRUE(test, wait_for_completion_timeout(&ctx->parked,
							    msecs_to_jiffies(LSV_HT_TEST_WAIT_MS)));
	for (n = LSV_HT_TEST_WIDE_HEAD; n < LSV_HT_TEST_WIDE_HEAD + fill; n++)
		KUNIT_ASSERT_NOT_NULL(test, lsv_ht_test_insert(ctx, keys[n], &is_new));

	/* Act */
	waiter->key = LSV_HT_TEST_CHURN_KEY;
	lsv_ht_test_job_start(ctx, waiter, 0, 0, ht_test_wait_recovery_fn);
	KUNIT_ASSERT_TRUE(test, lsv_ht_test_job_wait(waiter));
	KUNIT_ASSERT_GT(test, lsv_ht_recovery_cnt(ctx->ht), 0ULL);
	walked = ht_kunit_recovery_walked(ctx->ht);

	complete(&ctx->release);
	KUNIT_ASSERT_TRUE(test, lsv_ht_test_job_wait(stalled));
	kunit_deactivate_static_stub(test, ht_kunit_anchor_dropped);
	KUNIT_EXPECT_NOT_NULL(test, stalled->value);

	/* Assert: the anchor, the cells inserted behind it and the older ones after them */
	live = 1 + fill + LSV_HT_TEST_WIDE_TAIL;
	passes = DIV_ROUND_UP(live, LSV_DTA_SPARE_LIMIT);
	bound = (u64)passes * 3 * (total + 1);
	kunit_info(test, "range of %u live cells, %u passes, %llu cells walked, bound %llu\n",
		   live, passes, walked, bound);
	KUNIT_EXPECT_LE(test, walked, bound);
}

static void LSV_KTEST(ht_recovery_walks_short_range)(struct kunit *test)
{
	ht_test_recovery_walks(test, 2 * LSV_DTA_SPARE_LIMIT);
}

static void LSV_KTEST(ht_recovery_walks_long_range)(struct kunit *test)
{
	ht_test_recovery_walks(test, 8 * LSV_DTA_SPARE_LIMIT);
}

static struct kunit_case __cases[] = {
	LSV_KUNIT_CASE(ht_restore_links_callers_cell),
	LSV_KUNIT_CASE(ht_stalled_participant),
	LSV_KUNIT_CASE(ht_stalled_participant_full_pool),
	LSV_KUNIT_CASE_SLOW(ht_recovery_waits_for_cells),
	LSV_KUNIT_CASE(ht_stalled_at_bucket_head),
	LSV_KUNIT_CASE(ht_stalled_at_empty_bucket_head),
	LSV_KUNIT_CASE_SLOW(ht_stalled_range_outgrows_reserve),
	LSV_KUNIT_CASE_SLOW(ht_stalled_range_full_pool),
	LSV_KUNIT_CASE_SLOW(ht_recovery_walks_short_range),
	LSV_KUNIT_CASE_SLOW(ht_recovery_walks_long_range),
	LSV_KUNIT_CASE(ht_anchor_ends_with_the_op),
	LSV_KUNIT_CASE(ht_recovery_covers_the_anchor_window),
	LSV_KUNIT_CASE(ht_recovery_ignores_cells_linked_behind),
	LSV_KUNIT_CASE(ht_reclaim_reads_one_record_state),
	LSV_KUNIT_CASE_SLOW(ht_stale_begin_second_suspect),
	{}
};

static struct kunit_suite __suite = {
	.name		= "lsv_ht_dta_recovery",
	.init		= lsv_ht_test_init,
	.exit		= lsv_ht_test_exit,
	.test_cases	= __cases,
};

kunit_test_suites(&__suite);
