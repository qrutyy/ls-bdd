/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef _LSV_POOL_WQ_H
#define _LSV_POOL_WQ_H

#include <linux/cpumask.h>
#include <linux/workqueue.h>

/*
 * A thin layer over cmwq for work that may block (reads of the backing device,
 * GC, flush). Its workers are anonymous and come and go with the load, so a
 * job has no worker id: whatever needs one runs on lsv_shard_wq.
 */

#define lsv_pool_wq      workqueue_struct
#define lsv_pool_wq_work work_struct

/*
 * An unbound queue that keeps making progress under memory pressure.
 *
 * |worker_cnt| is the number of jobs of the queue that run at the same time.
 * <= 0 takes one for every online CPU of |cpumask|, as lsv_shard_wq_create()
 * does, or cmwq's default when |cpumask| is NULL. |cpumask| only sizes the
 * queue: a module cannot set the affinity of a single workqueue.
 *
 * Returns an ERR_PTR on failure, like lsv_shard_wq_create().
 */
__printf(3, 4) struct lsv_pool_wq *
lsv_pool_wq_create(int worker_cnt, const struct cpumask *cpumask, const char *namefmt, ...);

/* Runs the jobs already queued, then frees the queue. */
void lsv_pool_wq_destroy(struct lsv_pool_wq *wq);

/* Waits for every job queued on |wq| so far. */
void lsv_pool_wq_flush(struct lsv_pool_wq *wq);

/*
 * A macro, not a function: INIT_WORK() gives every expansion a lockdep class
 * of its own, and a function would make all jobs share one.
 */
#define lsv_pool_wq_init_work(work, func) INIT_WORK(work, func)

/* false if |work| is already queued and has not started yet. */
bool lsv_pool_wq_queue_work(struct lsv_pool_wq *wq, struct lsv_pool_wq_work *work);

#endif /* _LSV_POOL_WQ_H */
