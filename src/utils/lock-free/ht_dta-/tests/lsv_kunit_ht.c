#include "lsv_kunit_ht.h"

#include <linux/cpumask.h>
#include <linux/delay.h>
#include <linux/hash.h>
#include <linux/jiffies.h>
#include <linux/moduleparam.h>
#include <linux/random.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <kunit/test.h>

#include "include_static/__static_kunit_lsv_ht.h"

/*
 * Seed of the randomized stress cases; 0 draws one. A failure reports the
 * seed it ran with, and loading the module with that seed replays the keys
 * every participant picks -- though not the interleaving of the participants.
 */
static ulong ht_test_seed;
module_param(ht_test_seed, ulong, 0444);
MODULE_PARM_DESC(ht_test_seed, "seed of the lsv_ht KUnit stress cases, 0 for a random one");

/* A cell of the test ops; |fallback| is one that did not come from the pool. */
struct lsv_ht_test_cell {
	struct lsv_lf_list_node node;
	bool fallback;
	/* given back by the table, and in the quarantine */
	bool freed;
	struct list_head qlink;
};

/* The fixture of the running case, for the walk hook, which has no context. */
static struct lsv_ht_test_ctx *g_lsv_ht_test_ctx;

static struct lsv_ht_test_cell *ht_test_cell_of(struct lsv_lf_list_node *node)
{
	return container_of(node, struct lsv_ht_test_cell, node);
}

/*
 * |for_recovery| is node_copy()'s call: it reaches cells ordinary inserts
 * never get, so that a full table does not also block the recovery that has to
 * run to drain it, and past those, the fallback when the test turns it on.
 */
/* A cell given back earlier, wiped as a new one; NULL when there is none. */
static struct lsv_ht_test_cell *ht_test_cell_reuse(struct lsv_ht_test_ctx *ctx)
{
	struct lsv_ht_test_cell *cell = NULL;

	if (!ctx->reuse)
		return NULL;

	spin_lock(&ctx->quarantine_lock);
	if (!list_empty(&ctx->reusable)) {
		cell = list_first_entry(&ctx->reusable, struct lsv_ht_test_cell, qlink);
		list_del(&cell->qlink);
	}
	spin_unlock(&ctx->quarantine_lock);

	if (!cell)
		return NULL;

	memset(&cell->node, 0, sizeof(cell->node));
	cell->fallback = false;
	WRITE_ONCE(cell->freed, false);
	atomic_inc(&ctx->reused);
	return cell;
}

static struct lsv_ht_test_cell *ht_test_cell_get(struct lsv_ht_test_ctx *ctx)
{
	struct lsv_ht_test_cell *cell = ht_test_cell_reuse(ctx);

	return cell ? cell : kzalloc(sizeof(*cell), GFP_KERNEL);
}

static struct lsv_lf_list_node *ht_test_node_new(struct lsv_ht_test_ctx *ctx, bool for_recovery)
{
	int limit = ctx->nodes_max + (for_recovery ? ctx->recovery_max : 0);
	struct lsv_ht_test_cell *cell;

	if (atomic_inc_return(&ctx->nodes_live) > limit) {
		atomic_dec(&ctx->nodes_live);
		if (!for_recovery || !ctx->copy_fallback)
			return NULL;

		cell = ht_test_cell_get(ctx);
		if (!cell)
			return NULL;
		cell->fallback = true;
		atomic_inc(&ctx->fallback_live);
		atomic_inc(&ctx->fallback_cnt);
		return &cell->node;
	}

	cell = ht_test_cell_get(ctx);
	if (!cell) {
		atomic_dec(&ctx->nodes_live);
		return NULL;
	}
	return &cell->node;
}

/* True, and counted, when |node| is a cell the table has already given back. */
static bool ht_test_cell_is_freed(struct lsv_ht_test_ctx *ctx, struct lsv_lf_list_node *node)
{
	if (!READ_ONCE(ht_test_cell_of(node)->freed))
		return false;

	atomic_inc(&ctx->touched_freed);
	return true;
}

/* g_lsv_ht_cell_check: every cell a walk of the table steps on. */
static void ht_test_cell_check(struct lsv_lf_list_node *node)
{
	struct lsv_ht_test_ctx *ctx = READ_ONCE(g_lsv_ht_test_ctx);

	if (ctx)
		ht_test_cell_is_freed(ctx, node);
}

/*
 * Gives a cell back to where ht_test_node_new() took it from, and keeps its
 * memory in the quarantine.
 */
static void ht_test_cell_put(struct lsv_ht_test_ctx *ctx, struct lsv_lf_list_node *node)
{
	struct lsv_ht_test_cell *cell = ht_test_cell_of(node);

	if (cell->fallback)
		atomic_dec(&ctx->fallback_live);
	else
		atomic_dec(&ctx->nodes_live);

	if (node == READ_ONCE(ctx->held_cell))
		WRITE_ONCE(ctx->held_cell_freed, true);

	WRITE_ONCE(cell->freed, true);
	spin_lock(&ctx->quarantine_lock);
	/* reused ones go to the back, so that an address comes back soon but not at once */
	if (ctx->reuse)
		list_add_tail(&cell->qlink, &ctx->reusable);
	else
		list_add(&cell->qlink, &ctx->quarantine);
	spin_unlock(&ctx->quarantine_lock);
}

struct lsv_lf_list_node *lsv_ht_test_node_alloc(u64 key, void *priv)
{
	struct lsv_ht_test_ctx *ctx = priv;
	struct lsv_ht_test_value *value;
	struct lsv_lf_list_node *node;

	if (lsv_ht_key_is_reserved(key)) {
		atomic_inc(&ctx->sentinel_calls);
		return NULL;
	}

	node = ht_test_node_new(ctx, false);
	if (!node)
		return NULL;

	node->key = key;

	value = kzalloc(sizeof(*value), GFP_KERNEL);
	if (!value) {
		ht_test_cell_put(ctx, node);
		return NULL;
	}

	/* values outlive every cell; the fixture frees them after the table */
	refcount_set(&value->ref, 1);
	atomic_set(&value->cell_cnt, 1);
	value->key = key;
	INIT_LIST_HEAD(&value->key_link);
	spin_lock(&ctx->values_lock);
	list_add(&value->link, &ctx->values);
	spin_unlock(&ctx->values_lock);

	node->value = value;

	/*
	 * Parks the caller in the middle of an insertion, with its anchor
	 * wherever the traversal left it and its cell already in hand, so that
	 * whatever the others do to the pool meanwhile cannot fail it.
	 */
	if (atomic_xchg(&ctx->park_in_alloc, 0)) {
		complete(&ctx->parked);
		wait_for_completion(&ctx->release);
	}

	return node;
}

static struct lsv_lf_list_node *ht_test_node_copy(struct lsv_lf_list_node *src, void *priv)
{
	struct lsv_ht_test_ctx *ctx = priv;
	struct lsv_lf_list_node *node;

	if (lsv_ht_key_is_reserved(src->key)) {
		atomic_inc(&ctx->sentinel_calls);
		return NULL;
	}
	if (ht_test_cell_is_freed(ctx, src))
		return NULL;

	/* the cells a recovery may not have: see ht_recovery_waits_for_cells */
	if (atomic_read(&ctx->copy_blocked) &&
	    (!ctx->copy_block_max || src->key <= ctx->copy_block_max))
		return NULL;

	node = ht_test_node_new(ctx, true);
	if (!node)
		return NULL;

	node->key = src->key;
	node->value = src->value;
	atomic_inc(&((struct lsv_ht_test_value *)node->value)->cell_cnt);

	return node;
}

void lsv_ht_test_node_dealloc(struct lsv_lf_list_node *node, void *priv)
{
	struct lsv_ht_test_ctx *ctx = priv;
	struct lsv_ht_test_value *value;

	if (lsv_ht_key_is_reserved(node->key)) {
		atomic_inc(&ctx->sentinel_calls);
		return;
	}
	/* given back twice */
	if (ht_test_cell_is_freed(ctx, node))
		return;

	value = node->value;
	if (atomic_dec_and_test(&value->cell_cnt) && refcount_read(&value->ref) &&
	    !READ_ONCE(ctx->tearing_down) && !ctx->norefs)
		atomic_inc(&ctx->released_held);

	ht_test_cell_put(ctx, node);
	atomic_inc(&ctx->nodes_freed);
}

static refcount_t *ht_test_node_ref(struct lsv_lf_list_node *node, void *priv)
{
	struct lsv_ht_test_ctx *ctx = priv;

	/* a sentinel has no value to count references of */
	if (lsv_ht_key_is_reserved(node->key))
		atomic_inc(&ctx->sentinel_calls);
	ht_test_cell_is_freed(ctx, node);

	return &((struct lsv_ht_test_value *)node->value)->ref;
}

static bool ht_test_value_get(struct lsv_lf_list_node *node, void *priv)
{
	return refcount_inc_not_zero(ht_test_node_ref(node, priv));
}

static bool ht_test_value_is_dead(struct lsv_lf_list_node *node, void *priv)
{
	return !refcount_read(ht_test_node_ref(node, priv));
}

static void ht_test_value_put_new(struct lsv_lf_list_node *node, void *priv)
{
	WARN_ON(!refcount_dec_and_test(ht_test_node_ref(node, priv)));
}

static const struct lsv_lf_list_ops g_lsv_ht_test_ops = {
	.node_alloc	= lsv_ht_test_node_alloc,
	.node_copy	= ht_test_node_copy,
	.node_dealloc	= lsv_ht_test_node_dealloc,
	.value_get	= ht_test_value_get,
	.value_is_dead	= ht_test_value_is_dead,
	.value_put_new	= ht_test_value_put_new,
};

/* The same cells and values, but the table is not told about references. */
const struct lsv_lf_list_ops g_lsv_ht_test_ops_norefs = {
	.node_alloc	= lsv_ht_test_node_alloc,
	.node_copy	= ht_test_node_copy,
	.node_dealloc	= lsv_ht_test_node_dealloc,
};

static void ht_test_job_run(struct lsv_wq_ctx *wq_ctx)
{
	struct lsv_ht_test_job *job = container_of(wq_ctx, struct lsv_ht_test_job, wq_ctx);

	/* static stubs look the test up through current, which a worker lacks */
	if (job->stub_visible)
		current->kunit_test = job->ctx->test;
	job->fn(job);
	if (job->stub_visible)
		current->kunit_test = NULL;
	atomic_dec(&job->ctx->jobs_running);
	complete(&job->done);
}

/* The |worker|-th worker of a wq; lsv_wq_map() takes a CPU number. */
struct lsv_worker *lsv_ht_test_worker(struct lsv_ht_test_ctx *ctx, unsigned int wq_idx,
				      unsigned int worker)
{
	return lsv_wq_map(ctx->wq[wq_idx], NULL, cpumask_nth(worker, cpu_online_mask));
}

/*
 * A job is never on the stack of the test: an assertion that aborts the test
 * with a job still queued or parked leaves that job running, and KUnit frees
 * this only after .exit has joined the workers.
 */
struct lsv_ht_test_job *lsv_ht_test_job_new(struct kunit *test)
{
	struct lsv_ht_test_job *job = kunit_kzalloc(test, sizeof(*job), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, job);
	return job;
}

void lsv_ht_test_job_start_on(struct lsv_ht_test_ctx *ctx, struct lsv_ht_test_job *job,
			      struct lsv_worker *worker, lsv_ht_test_job_fn_t fn)
{
	job->ctx = ctx;
	job->fn = fn;
	init_completion(&job->done);
	lsv_wq_ctx_init(&job->wq_ctx, ht_test_job_run);
	atomic_inc(&ctx->jobs_running);
	lsv_worker_add(worker, &job->wq_ctx);
}

void lsv_ht_test_job_start(struct lsv_ht_test_ctx *ctx, struct lsv_ht_test_job *job,
			   unsigned int wq_idx, unsigned int worker,
			   lsv_ht_test_job_fn_t fn)
{
	lsv_ht_test_job_start_on(ctx, job, lsv_ht_test_worker(ctx, wq_idx, worker), fn);
}

bool lsv_ht_test_job_wait(struct lsv_ht_test_job *job)
{
	return wait_for_completion_timeout(&job->done, msecs_to_jiffies(LSV_HT_TEST_WAIT_MS));
}

/* Runs |fn| on one worker and waits for it. */
void lsv_ht_test_run(struct lsv_ht_test_ctx *ctx, struct lsv_ht_test_job *job,
		     lsv_ht_test_job_fn_t fn)
{
	lsv_ht_test_job_start(ctx, job, 0, 0, fn);
	KUNIT_ASSERT_TRUE(ctx->test, lsv_ht_test_job_wait(job));
}

void lsv_ht_test_insert_fn(struct lsv_ht_test_job *job)
{
	bool is_new;

	job->value = lsv_ht_insert_or_inc_ref(job->ctx->ht, job->key, &is_new);
	job->result = is_new;
}

void lsv_ht_test_find_fn(struct lsv_ht_test_job *job)
{
	job->value = lsv_ht_find_value(job->ctx->ht, job->key);
}

void *lsv_ht_test_insert(struct lsv_ht_test_ctx *ctx, u64 key, bool *is_new)
{
	struct lsv_ht_test_job *job = lsv_ht_test_job_new(ctx->test);

	job->key = key;
	lsv_ht_test_run(ctx, job, lsv_ht_test_insert_fn);
	*is_new = job->result;
	return job->value;
}

void *lsv_ht_test_find(struct lsv_ht_test_ctx *ctx, u64 key)
{
	struct lsv_ht_test_job *job = lsv_ht_test_job_new(ctx->test);

	job->key = key;
	lsv_ht_test_run(ctx, job, lsv_ht_test_find_fn);
	return job->value;
}

/*
 * An insertion retried for a while: one that finds no cell has the other
 * participants scan their buffers, and the cells come back on their workers,
 * for a later insertion. Returns the value pinned, or NULL if none came.
 */
struct lsv_ht_test_value *lsv_ht_test_insert_eventually(struct lsv_ht_test_ctx *ctx, u64 key)
{
	struct lsv_ht_test_value *value;
	unsigned int n;
	bool is_new;

	for (n = 0; n < 1000; n++) {
		value = lsv_ht_test_insert(ctx, key, &is_new);
		if (value)
			return value;
		usleep_range(1000, 2000);
	}
	return NULL;
}

/*
 * Records |value|, which an insertion has just published, as the one its key
 * stands for, and returns how many other values of the key are live as well.
 * A key stands for one value at a time, however many cells recoveries made
 * for it. A value that has lost its last reference can never be pinned again
 * and leaves the chain; one that still has any was never dropped, so it is
 * still in the table beside the new one.
 */
unsigned int lsv_ht_test_value_published(struct lsv_ht_test_ctx *ctx,
					 struct lsv_ht_test_value *value)
{
	struct list_head *chain = &ctx->live_by_key[hash_64(value->key, LSV_HT_TEST_KEY_BITS)];
	unsigned int live = 0;
	struct lsv_ht_test_value *other;
	struct lsv_ht_test_value *tmp;

	spin_lock(&ctx->values_lock);
	list_for_each_entry_safe(other, tmp, chain, key_link) {
		if (!refcount_read(&other->ref))
			list_del_init(&other->key_link);
		else if (other->key == value->key)
			live++;
	}
	list_add(&value->key_link, chain);
	spin_unlock(&ctx->values_lock);

	return live;
}

/* Values some reference is still held on; once every job has put its pins, none. */
unsigned int lsv_ht_test_values_held(struct lsv_ht_test_ctx *ctx)
{
	unsigned int held = 0;
	struct lsv_ht_test_value *value;

	spin_lock(&ctx->values_lock);
	list_for_each_entry(value, &ctx->values, link)
		held += !!refcount_read(&value->ref);
	spin_unlock(&ctx->values_lock);

	return held;
}

/* The seed of a randomized case: the module parameter, or a fresh one. */
u64 lsv_ht_test_seed_get(struct kunit *test)
{
	u64 seed = ht_test_seed ? ht_test_seed : get_random_u64();

	kunit_info(test, "seed=%llu\n", seed);
	return seed;
}

int lsv_ht_test_create_table_ops(struct lsv_ht_test_ctx *ctx, u64 size, int cells,
				 int recovery_cells, const struct lsv_lf_list_ops *ops)
{
	ctx->nodes_max = cells;
	ctx->recovery_max = recovery_cells;
	ctx->norefs = !ops->value_get;
	ctx->ht = lsv_ht_alloc_and_init(size, ctx, ops);
	if (IS_ERR(ctx->ht)) {
		int rc = PTR_ERR(ctx->ht);

		ctx->ht = NULL;
		return rc;
	}
	return 0;
}

int lsv_ht_test_create_table(struct lsv_ht_test_ctx *ctx, u64 size, int cells,
			     int recovery_cells)
{
	return lsv_ht_test_create_table_ops(ctx, size, cells, recovery_cells, &g_lsv_ht_test_ops);
}

/*
 * Opens every gate a job may be parked on and waits for the jobs; true when
 * every one is over within LSV_HT_TEST_WAIT_MS. A test that failed an assertion
 * may have left a job parked, and a stub that parks later passes straight
 * through an open gate.
 */
bool lsv_ht_test_join(struct lsv_ht_test_ctx *ctx)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(LSV_HT_TEST_WAIT_MS);

	complete_all(&ctx->release);
	complete_all(&ctx->link_release);
	complete_all(&ctx->record_release);
	complete_all(&ctx->release2);
	complete_all(&ctx->begin_release);
	complete_all(&ctx->hold_release);

	while (atomic_read(&ctx->jobs_running) && time_before(jiffies, deadline))
		usleep_range(1000, 2000);
	return !atomic_read(&ctx->jobs_running);
}

int lsv_ht_test_init(struct kunit *test)
{
	struct lsv_ht_test_ctx *ctx;
	unsigned int i;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->test = test;
	spin_lock_init(&ctx->values_lock);
	INIT_LIST_HEAD(&ctx->values);
	for (i = 0; i < ARRAY_SIZE(ctx->live_by_key); i++)
		INIT_LIST_HEAD(&ctx->live_by_key[i]);
	/* .exit opens every gate, whether the test got to arm it or not */
	init_completion(&ctx->parked);
	init_completion(&ctx->release);
	init_completion(&ctx->link_parked);
	init_completion(&ctx->link_release);
	init_completion(&ctx->record_parked);
	init_completion(&ctx->record_release);
	init_completion(&ctx->parked2);
	init_completion(&ctx->release2);
	init_completion(&ctx->begin_parked);
	init_completion(&ctx->begin_release);
	init_completion(&ctx->hold_parked);
	init_completion(&ctx->hold_release);
	spin_lock_init(&ctx->quarantine_lock);
	INIT_LIST_HEAD(&ctx->quarantine);
	INIT_LIST_HEAD(&ctx->reusable);
	test->priv = ctx;
	WRITE_ONCE(g_lsv_ht_test_ctx, ctx);
	WRITE_ONCE(g_lsv_ht_cell_check, ht_test_cell_check);

	for (i = 0; i < LSV_HT_TEST_WQ_CNT; i++) {
		ctx->wq[i] = lsv_wq_create(0, cpu_online_mask, "kunit-ht%u", i);
		if (IS_ERR(ctx->wq[i])) {
			int rc = PTR_ERR(ctx->wq[i]);

			ctx->wq[i] = NULL;
			return rc;
		}
	}
	ctx->worker_cnt = num_online_cpus();

	return 0;
}

void lsv_ht_test_exit(struct kunit *test)
{
	struct lsv_ht_test_ctx *ctx = test->priv;
	struct lsv_ht_test_value *value;
	struct lsv_ht_test_value *tmp;
	struct lsv_ht_test_cell *cell;
	struct lsv_ht_test_cell *next;
	unsigned long deadline;
	unsigned int i;

	if (!ctx)
		return;

	/*
	 * Every job ends on its own once the gates are open, so one that does
	 * not is stuck in the table. That is reported, and then waited for all
	 * the same: the jobs and this fixture are memory of the test, which the
	 * job would go on using, and destroying its queue would wait for it
	 * anyway. Only the timeout of the test runner ends such a hang.
	 */
	KUNIT_EXPECT_TRUE_MSG(test, lsv_ht_test_join(ctx),
			      "a job is stuck in the table: waiting for it without a bound");
	while (atomic_read(&ctx->jobs_running))
		usleep_range(10000, 11000);

	/*
	 * The table goes first, while the workers still run: lsv_ht_free()
	 * waits for the reclaim requests queued on them, which a destroyed
	 * queue would never run. A request that does not run is reported the
	 * same way as a job.
	 */
	deadline = jiffies + msecs_to_jiffies(LSV_HT_TEST_WAIT_MS);
	while (ctx->ht && ht_reclaim_requests_pending(ctx->ht) && time_before(jiffies, deadline))
		usleep_range(1000, 2000);
	KUNIT_EXPECT_FALSE_MSG(test, ctx->ht && ht_reclaim_requests_pending(ctx->ht),
			       "a reclaim request is stuck: waiting for it without a bound");

	KUNIT_EXPECT_EQ(test, atomic_read(&ctx->released_held), 0);
	WRITE_ONCE(ctx->tearing_down, true);
	lsv_ht_free(ctx->ht);

	for (i = 0; i < LSV_HT_TEST_WQ_CNT; i++)
		lsv_wq_destroy(ctx->wq[i]);
	WRITE_ONCE(g_lsv_ht_cell_check, NULL);
	WRITE_ONCE(g_lsv_ht_test_ctx, NULL);
	KUNIT_EXPECT_EQ(test, atomic_read(&ctx->nodes_live), 0);
	KUNIT_EXPECT_EQ(test, atomic_read(&ctx->fallback_live), 0);
	KUNIT_EXPECT_EQ(test, atomic_read(&ctx->sentinel_calls), 0);

	/* no walk and no op ever came back to a cell the table had given back */
	KUNIT_EXPECT_EQ(test, atomic_read(&ctx->touched_freed), 0);
	list_for_each_entry_safe(cell, next, &ctx->quarantine, qlink)
		kfree(cell);
	list_for_each_entry_safe(cell, next, &ctx->reusable, qlink)
		kfree(cell);

	list_for_each_entry_safe(value, tmp, &ctx->values, link) {
		/*
		 * Every cell that ever pointed at this value was announced and
		 * given back: one node_alloc() or node_copy() for each
		 * node_dealloc(), and nothing in between.
		 */
		KUNIT_EXPECT_EQ(test, atomic_read(&value->cell_cnt), 0);
		kfree(value);
	}
}
