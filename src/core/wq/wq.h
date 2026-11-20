/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef _LSV_WQ_H
#define _LSV_WQ_H

#include <linux/bug.h>
#include <linux/printk.h>

#include "core/wq/pool_wq.h"
#include "core/wq/shard_wq.h"

/*
 * The two classes of work:
 *  - shard: short, never blocks, runs on a worker with an id (index operations,
 *    SMR reclaim requests). Both modes run it on lsv_shard_wq.
 *  - pool: may block (reads of the backing device, GC, flush). Only HYBRID has
 *    a queue for it, on cmwq.
 *
 * A job is of one class for good: it is initialised as that class and queued
 * only as that class. Work that goes from one class to the other (a verify
 * that ends in an index operation) takes a job of each.
 */

/* TODO(qrutyy): translate mode to type */
enum lsv_wq_mode {
	LSV_WQ_MODE_PINNED,
	LSV_WQ_MODE_POOL,
	LSV_WQ_MODE_HYBRID,
};

enum lsv_wq_work_type {
	LSV_WQ_WORK_POOL,
	LSV_WQ_WORK_SHARD,
};

struct lsv_wq_work {
	enum lsv_wq_work_type type;

	union {
		struct lsv_pool_wq_work pool_work;
		struct lsv_shard_wq_work shard_work;
	};
};

struct lsv_wq {
	enum lsv_wq_mode mode;

	/* in both modes */
	struct lsv_shard_wq *shard;
	/* HYBRID only, NULL otherwise */
	struct lsv_pool_wq *pool;
};

struct lsv_wq *lsv_wq_create(enum lsv_wq_mode mode, const char *name);
void lsv_wq_destroy(struct lsv_wq *wq);

static inline void lsv_wq_init_work_pool(struct lsv_wq_work *work, lsv_pool_wq_func_t fn)
{
	work->type = LSV_WQ_WORK_POOL;
	lsv_pool_wq_init_work(&work->pool_work, fn);
}

/* A pool job that queues itself again |period| jiffies after |fn| returns. */
static inline void lsv_wq_init_periodic_work_pool(struct lsv_wq_work *work, lsv_pool_wq_func_t fn,
						  unsigned long period)
{
	work->type = LSV_WQ_WORK_POOL;
	lsv_pool_wq_init_periodic_work(&work->pool_work, fn, period);
}

static inline void lsv_wq_init_work_shard(struct lsv_wq_work *work, lsv_shard_wq_func_t fn)
{
	work->type = LSV_WQ_WORK_SHARD;
	lsv_shard_wq_init_work(&work->shard_work, fn);
}

/*
 * Queues |work| on the shard worker with |id|, from anywhere: the way into a
 * shard from the pool, or from outside of any worker.
 *
 * false: no such worker, or |work| is already queued and has not started yet.
 */
static inline bool lsv_wq_sched_shard_by_id(unsigned int id, struct lsv_wq_work *work)
{
	if (WARN_ON_ONCE(work->type != LSV_WQ_WORK_SHARD))
		return false;

	return lsv_shard_wq_queue_work_by_id(id, &work->shard_work);
}

/*
 * Queues |work| on the shard worker the caller runs on: the next step of a job
 * that stays where its data is.
 *
 * false: the caller is not a shard worker, or |work| is already queued and has
 * not started yet.
 */
static inline bool lsv_wq_sched_shard_current(struct lsv_wq_work *work)
{
	if (WARN_ON_ONCE(work->type != LSV_WQ_WORK_SHARD))
		return false;

	return lsv_shard_wq_queue_work_current(&work->shard_work);
}

/*
 * |delay| is in jiffies, 0 runs |work| at once. For a periodic job it starts the cycle.
 *
 * false: |work| is already queued, or PINNED has no queue for work that blocks.
 */
static inline bool lsv_wq_sched_pool(struct lsv_wq *wq, struct lsv_wq_work *work, unsigned long delay)
{
	if (WARN_ON_ONCE(work->type != LSV_WQ_WORK_POOL))
		return false;

	if (wq->mode == LSV_WQ_MODE_PINNED) {
		pr_warn_once("lsv: LSV_WQ_MODE_PINNED has no pool queue, work is not scheduled\n");
		return false;
	}

	return lsv_pool_wq_queue_work(wq->pool, &work->pool_work, delay);
}

/* Stops a periodic pool job and waits for its run in progress, see lsv_pool_wq_cancel_work(). */
static inline void lsv_wq_cancel_pool(struct lsv_wq_work *work)
{
	if (WARN_ON_ONCE(work->type != LSV_WQ_WORK_POOL))
		return;

	lsv_pool_wq_cancel_work(&work->pool_work);
}

#endif /* _LSV_WQ_H */
