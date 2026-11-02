// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>
#include <linux/completion.h>
#include <linux/err.h>
#include <linux/math64.h>
#include <linux/sched/clock.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

#include "utils/ds_control.h"
#include "utils/kunit.h"

#ifdef LF_MODE
#include "utils/wq.h"
#endif

/*
 * The contract every mapping index registered in ds_control.c has to keep,
 * run against each of them in turn: a new index is tested by registering it.
 * What indexes may differ in (who frees values, order, removal of absent keys)
 * they declare in lsv_ds_ops.caps, and the cases follow the declaration.
 *
 * The lock-free hashtable only accepts operations from lsv_wq workers created
 * before it, so in LF_MODE every index operation runs on a worker of the
 * fixture; checks stay on the test thread.
 *
 * lsv_ds_bench is not a regression test: it reports single-thread ns/op of
 * every index with kunit_info() and fails only when it could not run.
 */

#define LSV_DSCT_KEYS 512
#define LSV_DSB_KEYS 65536
/* odd, so multiplying by it permutes every power-of-two range */
#define LSV_DS_SPREAD 0x9e3779b97f4a7c15ULL
/* keys of the spread cases stay below this, as logical cell numbers do */
#define LSV_DS_KEY_BITS 40

struct lsv_ds_val {
	u64 key;
};

struct lsv_ds_ct {
	const struct lsv_ds_ops *ops;

	struct lsv_ds ds;
	bool ds_live;
	s32 init_rc;
	bool empty;

	struct lsv_cache_mng cache_mng;
	struct lsv_lmap_cache map_cache;

	/* the batch the worker runs next, and what it found */
	u32 n;
	sector_t *keys;
	struct lsv_ds_val **vals; /* values the index holds and the test still owns */
	void **found;
	s32 *rc;
	bool *pick; /* which keys a remove batch takes */

#ifdef LF_MODE
	struct lsv_wq *wq;
#endif
};

typedef void (*lsv_ds_batch_t)(struct lsv_ds_ct *ct);

#ifdef LF_MODE
struct lsv_ds_ct_job {
	struct lsv_wq_ctx ctx;
	struct lsv_ds_ct *ct;
	lsv_ds_batch_t fn;
	struct completion done;
};

static void lsv_ds_ct_job_fn(struct lsv_wq_ctx *ctx)
{
	struct lsv_ds_ct_job *job = container_of(ctx, struct lsv_ds_ct_job, ctx);

	job->fn(job->ct);
	complete(&job->done);
}
#endif

static void lsv_ds_ct_run(struct lsv_ds_ct *ct, lsv_ds_batch_t fn)
{
#ifdef LF_MODE
	struct lsv_ds_ct_job job = { .ct = ct, .fn = fn };

	init_completion(&job.done);
	lsv_wq_ctx_init(&job.ctx, lsv_ds_ct_job_fn);
	lsv_worker_add(lsv_wq_map(ct->wq, NULL, 0), &job.ctx);
	wait_for_completion(&job.done);
#else
	fn(ct);
#endif
}

static void lsv_ds_ct_b_init(struct lsv_ds_ct *ct)
{
	ct->init_rc = lsv_ds_init(&ct->ds, ct->ops->name, &ct->cache_mng, 4 * max(ct->n, (u32)LSV_DSCT_KEYS));
	ct->ds_live = !ct->init_rc;
}

static void lsv_ds_ct_b_insert(struct lsv_ds_ct *ct)
{
	struct lsv_ds_val *v;
	u32 i;

	for (i = 0; i < ct->n; i++) {
		v = kmem_cache_alloc(ct->map_cache.cell_cachep, GFP_KERNEL);
		if (!v) {
			ct->rc[i] = -ENOMEM;
			continue;
		}

		v->key = ct->keys[i];
		ct->rc[i] = lsv_ds_insert(&ct->ds, ct->keys[i], v, &ct->map_cache);
		if (ct->rc[i])
			kmem_cache_free(ct->map_cache.cell_cachep, v);
		else
			ct->vals[i] = v;
	}
}

static void lsv_ds_ct_b_lookup(struct lsv_ds_ct *ct)
{
	u32 i;

	for (i = 0; i < ct->n; i++)
		ct->found[i] = lsv_ds_lookup(&ct->ds, ct->keys[i]);
}

static void lsv_ds_ct_b_empty(struct lsv_ds_ct *ct)
{
	ct->empty = lsv_ds_empty_check(&ct->ds);
}

static void lsv_ds_ct_b_remove(struct lsv_ds_ct *ct)
{
	bool owns = ct->ops->caps & LSV_DS_REMOVE_OWNS_VALUE;
	u32 i;

	for (i = 0; i < ct->n; i++) {
		if (!ct->pick[i])
			continue;

		lsv_ds_remove(&ct->ds, ct->keys[i], ct->map_cache.cell_cachep);
		if (ct->vals[i] && !owns)
			kmem_cache_free(ct->map_cache.cell_cachep, ct->vals[i]);
		ct->vals[i] = NULL;
	}
}

static void lsv_ds_ct_b_free(struct lsv_ds_ct *ct)
{
	bool owns = ct->ops->caps & LSV_DS_FREE_OWNS_VALUES;
	u32 i;

	lsv_ds_free(&ct->ds, &ct->map_cache);
	ct->ds_live = false;

	for (i = 0; i < ct->n; i++) {
		if (ct->vals[i] && !owns)
			kmem_cache_free(ct->map_cache.cell_cachep, ct->vals[i]);
		ct->vals[i] = NULL;
	}
}

/* |n| keys spread over [1, 2^LSV_DS_KEY_BITS), all distinct and none reserved. */
static void lsv_ds_ct_spread_keys(struct lsv_ds_ct *ct, u32 n)
{
	u64 mask = (1ULL << LSV_DS_KEY_BITS) - 1;
	u32 i;

	ct->n = n;
	for (i = 0; i < n; i++)
		ct->keys[i] = (((u64)i * LSV_DS_SPREAD) & (mask >> 1)) * 2 + 1;
}

static s32 lsv_ds_ct_alloc_arrays(struct lsv_ds_ct *ct, u32 n)
{
	ct->keys = kvcalloc(n, sizeof(*ct->keys), GFP_KERNEL);
	ct->vals = kvcalloc(n, sizeof(*ct->vals), GFP_KERNEL);
	ct->found = kvcalloc(n, sizeof(*ct->found), GFP_KERNEL);
	ct->rc = kvcalloc(n, sizeof(*ct->rc), GFP_KERNEL);
	ct->pick = kvcalloc(n, sizeof(*ct->pick), GFP_KERNEL);

	return ct->keys && ct->vals && ct->found && ct->rc && ct->pick ? 0 : -ENOMEM;
}

static void lsv_ds_ct_free_arrays(struct lsv_ds_ct *ct)
{
	kvfree(ct->keys);
	kvfree(ct->vals);
	kvfree(ct->found);
	kvfree(ct->rc);
	kvfree(ct->pick);
}

static const void *lsv_ds_ct_gen(struct kunit *test, const void *prev, char *desc)
{
	const struct lsv_ds_ops *ops;
	u32 i = 0;

	if (prev) {
		while (lsv_ds_ops_at(i) != prev)
			i++;
		i++;
	}

	ops = lsv_ds_ops_at(i);
	if (ops)
		snprintf(desc, KUNIT_PARAM_DESC_SIZE, "%s", ops->name);

	return ops;
}

static int lsv_ds_ct_init(struct kunit *test)
{
	struct lsv_ds_ct *ct;

	ct = kzalloc(sizeof(*ct), GFP_KERNEL);
	if (!ct)
		return -ENOMEM;
	test->priv = ct;

	/* the registry case has no parameter and no index */
	ct->ops = test->param_value;
	if (!ct->ops)
		return 0;

	if (lsv_ds_ct_alloc_arrays(ct, LSV_DSB_KEYS))
		return -ENOMEM;

	ct->map_cache.entry_cache_mng = &ct->cache_mng;
	ct->map_cache.cell_cachep = kmem_cache_create("lsv_kunit_ds_val", sizeof(struct lsv_ds_val), 0, 0, NULL);
	if (!ct->map_cache.cell_cachep)
		return -ENOMEM;

#ifdef LF_MODE
	/* before the index: the hashtable takes its participants from the workers that exist */
	ct->wq = lsv_wq_create(1, cpu_online_mask, "kunit-lsv-ds");
	if (IS_ERR(ct->wq)) {
		s32 rc = PTR_ERR(ct->wq);

		ct->wq = NULL;
		return rc;
	}
#endif

	return 0;
}

static void lsv_ds_ct_exit(struct kunit *test)
{
	struct lsv_ds_ct *ct = test->priv;
	struct kmem_cache **caches[] = {
		&ct->cache_mng.ht_cache,
		&ct->cache_mng.sl_cache,
		&ct->cache_mng.bt_cache,
		&ct->cache_mng.rb_cache,
	};
	u32 i;

	if (!ct)
		return;

	if (ct->ds_live)
		lsv_ds_ct_run(ct, lsv_ds_ct_b_free);

#ifdef LF_MODE
	if (ct->wq)
		lsv_wq_destroy(ct->wq);
#endif

	for (i = 0; i < ARRAY_SIZE(caches); i++)
		kmem_cache_destroy(*caches[i]);
	kmem_cache_destroy(ct->map_cache.cell_cachep);

	lsv_ds_ct_free_arrays(ct);
	kfree(ct);
}

/* Inits the index of the case, sized for |n| keys. */
static void lsv_ds_ct_open(struct kunit *test, u32 n)
{
	struct lsv_ds_ct *ct = test->priv;

	ct->n = n;
	lsv_ds_ct_run(ct, lsv_ds_ct_b_init);
	KUNIT_ASSERT_EQ_MSG(test, ct->init_rc, 0, "%s: init failed", ct->ops->name);
}

static void LSV_KTEST(ds_registry)(struct kunit *test)
{
	const struct lsv_ds_ops *ops, *other;
	u32 i, j;

	KUNIT_ASSERT_NOT_NULL(test, lsv_ds_ops_at(0));

	for (i = 0; (ops = lsv_ds_ops_at(i)); i++) {
		KUNIT_EXPECT_GT(test, strlen(ops->name), 0);
		/* configfs takes names this long at most */
		KUNIT_EXPECT_LE(test, strlen(ops->name), 2);
		KUNIT_EXPECT_PTR_EQ(test, lsv_ds_find(ops->name), ops);
		KUNIT_EXPECT_TRUE(test, lsv_ds_check_available(ops->name));

		KUNIT_EXPECT_NOT_NULL(test, ops->init);
		KUNIT_EXPECT_NOT_NULL(test, ops->free);
		KUNIT_EXPECT_NOT_NULL(test, ops->lookup);
		KUNIT_EXPECT_NOT_NULL(test, ops->insert);
		KUNIT_EXPECT_NOT_NULL(test, ops->remove);
		KUNIT_EXPECT_NOT_NULL(test, ops->last);
		KUNIT_EXPECT_NOT_NULL(test, ops->prev);
		KUNIT_EXPECT_NOT_NULL(test, ops->empty);

		for (j = 0; j < i; j++) {
			other = lsv_ds_ops_at(j);
			KUNIT_EXPECT_STRNEQ(test, other->name, ops->name);
		}
	}

	KUNIT_EXPECT_NULL(test, lsv_ds_find("zz"));
	KUNIT_EXPECT_NULL(test, lsv_ds_find(""));
	KUNIT_EXPECT_NULL(test, lsv_ds_find(NULL));
	KUNIT_EXPECT_FALSE(test, lsv_ds_check_available("h"));
}

static void LSV_KTEST(ds_starts_empty)(struct kunit *test)
{
	struct lsv_ds_ct *ct = test->priv;
	u32 i;

	lsv_ds_ct_open(test, 16);

	lsv_ds_ct_run(ct, lsv_ds_ct_b_empty);
	KUNIT_EXPECT_TRUE(test, ct->empty);

	lsv_ds_ct_spread_keys(ct, 16);
	lsv_ds_ct_run(ct, lsv_ds_ct_b_lookup);
	for (i = 0; i < ct->n; i++)
		KUNIT_EXPECT_NULL(test, ct->found[i]);
}

static void LSV_KTEST(ds_insert_lookup)(struct kunit *test)
{
	struct lsv_ds_ct *ct = test->priv;
	u32 i;

	lsv_ds_ct_open(test, LSV_DSCT_KEYS);
	lsv_ds_ct_spread_keys(ct, LSV_DSCT_KEYS);

	lsv_ds_ct_run(ct, lsv_ds_ct_b_insert);
	for (i = 0; i < ct->n; i++)
		KUNIT_ASSERT_EQ_MSG(test, ct->rc[i], 0, "insert of key %llu", (u64)ct->keys[i]);

	lsv_ds_ct_run(ct, lsv_ds_ct_b_lookup);
	for (i = 0; i < ct->n; i++) {
		KUNIT_ASSERT_PTR_EQ_MSG(test, ct->found[i], (void *)ct->vals[i], "lookup of key %llu",
					(u64)ct->keys[i]);
		KUNIT_EXPECT_EQ(test, ((struct lsv_ds_val *)ct->found[i])->key, (u64)ct->keys[i]);
	}

	lsv_ds_ct_run(ct, lsv_ds_ct_b_empty);
	KUNIT_EXPECT_FALSE(test, ct->empty);
}

static void LSV_KTEST(ds_lookup_absent)(struct kunit *test)
{
	struct lsv_ds_ct *ct = test->priv;
	u32 half = LSV_DSCT_KEYS / 2;
	u32 i;

	lsv_ds_ct_open(test, LSV_DSCT_KEYS);
	lsv_ds_ct_spread_keys(ct, LSV_DSCT_KEYS);

	/* the first half goes in, the second is looked up and must not be found */
	ct->n = half;
	lsv_ds_ct_run(ct, lsv_ds_ct_b_insert);
	for (i = 0; i < half; i++)
		KUNIT_ASSERT_EQ(test, ct->rc[i], 0);

	ct->n = LSV_DSCT_KEYS;
	lsv_ds_ct_run(ct, lsv_ds_ct_b_lookup);
	for (i = half; i < LSV_DSCT_KEYS; i++)
		KUNIT_EXPECT_NULL_MSG(test, ct->found[i], "key %llu was never inserted", (u64)ct->keys[i]);
	for (i = 0; i < half; i++)
		KUNIT_EXPECT_PTR_EQ(test, ct->found[i], (void *)ct->vals[i]);
}

/* Logical cell numbers of small and very large devices alike, lba 0 included (key 1). */
static void LSV_KTEST(ds_wide_keys)(struct kunit *test)
{
	static const sector_t keys[] = {
		1, 2, 3, 4095, 4096, 1ULL << 20, (1ULL << 32) - 1, 1ULL << 32, (1ULL << 32) + 1, 1ULL << 40, (1ULL << 47) - 1,
	};
	struct lsv_ds_ct *ct = test->priv;
	u32 i;

	lsv_ds_ct_open(test, ARRAY_SIZE(keys));
	for (i = 0; i < ARRAY_SIZE(keys); i++)
		ct->keys[i] = keys[i];
	ct->n = ARRAY_SIZE(keys);

	lsv_ds_ct_run(ct, lsv_ds_ct_b_insert);
	for (i = 0; i < ct->n; i++)
		KUNIT_EXPECT_EQ_MSG(test, ct->rc[i], 0, "insert of key %llu", (u64)keys[i]);

	lsv_ds_ct_run(ct, lsv_ds_ct_b_lookup);
	for (i = 0; i < ct->n; i++)
		KUNIT_EXPECT_PTR_EQ_MSG(test, ct->found[i], (void *)ct->vals[i], "lookup of key %llu", (u64)keys[i]);
}

static void LSV_KTEST(ds_remove)(struct kunit *test)
{
	struct lsv_ds_ct *ct = test->priv;
	u32 i;

	lsv_ds_ct_open(test, LSV_DSCT_KEYS);
	lsv_ds_ct_spread_keys(ct, LSV_DSCT_KEYS);

	lsv_ds_ct_run(ct, lsv_ds_ct_b_insert);
	for (i = 0; i < ct->n; i++)
		KUNIT_ASSERT_EQ(test, ct->rc[i], 0);

	for (i = 0; i < ct->n; i++)
		ct->pick[i] = !(i % 2);
	lsv_ds_ct_run(ct, lsv_ds_ct_b_remove);

	lsv_ds_ct_run(ct, lsv_ds_ct_b_lookup);
	for (i = 0; i < ct->n; i++) {
		if (ct->pick[i])
			KUNIT_EXPECT_NULL_MSG(test, ct->found[i], "key %llu was removed", (u64)ct->keys[i]);
		else
			KUNIT_EXPECT_PTR_EQ_MSG(test, ct->found[i], (void *)ct->vals[i], "key %llu was not removed",
						(u64)ct->keys[i]);
	}

	/* the removed half once more: absent keys now, for the indexes that allow it */
	if (ct->ops->caps & LSV_DS_REMOVE_ABSENT_OK) {
		lsv_ds_ct_run(ct, lsv_ds_ct_b_remove);
		lsv_ds_ct_run(ct, lsv_ds_ct_b_lookup);
		for (i = 1; i < ct->n; i += 2)
			KUNIT_EXPECT_PTR_EQ(test, ct->found[i], (void *)ct->vals[i]);
	}

	for (i = 0; i < ct->n; i++)
		ct->pick[i] = i % 2;
	lsv_ds_ct_run(ct, lsv_ds_ct_b_remove);

	lsv_ds_ct_run(ct, lsv_ds_ct_b_empty);
	KUNIT_EXPECT_TRUE_MSG(test, ct->empty, "every key was removed");
}

/* Benchmark: the batch the worker times, and its result. */
static u64 lsv_dsb_ns;

static void lsv_dsb_b_insert(struct lsv_ds_ct *ct)
{
	u64 start = local_clock();

	lsv_ds_ct_b_insert(ct);
	lsv_dsb_ns = local_clock() - start;
}

static void lsv_dsb_b_lookup(struct lsv_ds_ct *ct)
{
	u64 start = local_clock();

	lsv_ds_ct_b_lookup(ct);
	lsv_dsb_ns = local_clock() - start;
}

static void lsv_dsb_one(struct kunit *test, const char *order)
{
	struct lsv_ds_ct *ct = test->priv;
	u64 insert, hit, miss;
	u32 i;

	lsv_ds_ct_run(ct, lsv_dsb_b_insert);
	for (i = 0; i < ct->n; i++)
		KUNIT_ASSERT_EQ(test, ct->rc[i], 0);
	insert = lsv_dsb_ns;

	lsv_ds_ct_run(ct, lsv_dsb_b_lookup);
	hit = lsv_dsb_ns;
	for (i = 0; i < ct->n; i++)
		KUNIT_ASSERT_PTR_EQ(test, ct->found[i], (void *)ct->vals[i]);

	/* keys above every inserted one: the same walk, but nothing at its end */
	for (i = 0; i < ct->n; i++)
		ct->keys[i] |= 1ULL << (LSV_DS_KEY_BITS + 1);
	lsv_ds_ct_run(ct, lsv_dsb_b_lookup);
	miss = lsv_dsb_ns;

	kunit_info(test, "lsv_ds_bench ds=%s order=%s keys=%u insert_ns=%llu lookup_hit_ns=%llu lookup_miss_ns=%llu\n",
		   ct->ops->name, order, ct->n, div_u64(insert, ct->n), div_u64(hit, ct->n), div_u64(miss, ct->n));
}

static void LSV_KTEST(ds_bench)(struct kunit *test)
{
	struct lsv_ds_ct *ct = test->priv;
	u32 i;

	lsv_ds_ct_open(test, LSV_DSB_KEYS);
	ct->n = LSV_DSB_KEYS;
	for (i = 0; i < ct->n; i++)
		ct->keys[i] = i + 1;
	lsv_dsb_one(test, "seq");

	lsv_ds_ct_run(ct, lsv_ds_ct_b_free);
	lsv_ds_ct_open(test, LSV_DSB_KEYS);
	lsv_ds_ct_spread_keys(ct, LSV_DSB_KEYS);
	lsv_dsb_one(test, "spread");
}

static struct kunit_case __cases[] = {
	LSV_KUNIT_CASE(ds_registry),
	KUNIT_CASE_PARAM(LSV_KTEST(ds_starts_empty), lsv_ds_ct_gen),
	KUNIT_CASE_PARAM(LSV_KTEST(ds_insert_lookup), lsv_ds_ct_gen),
	KUNIT_CASE_PARAM(LSV_KTEST(ds_lookup_absent), lsv_ds_ct_gen),
	KUNIT_CASE_PARAM(LSV_KTEST(ds_wide_keys), lsv_ds_ct_gen),
	KUNIT_CASE_PARAM(LSV_KTEST(ds_remove), lsv_ds_ct_gen),
	KUNIT_CASE_PARAM_ATTR(LSV_KTEST(ds_bench), lsv_ds_ct_gen, { .speed = KUNIT_SPEED_SLOW }),
	{}
};

static struct kunit_suite __suite = {
	.name = "lsv_ds",
	.init = lsv_ds_ct_init,
	.exit = lsv_ds_ct_exit,
	.test_cases = __cases,
};

kunit_test_suites(&__suite);
