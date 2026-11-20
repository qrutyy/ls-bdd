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

#define lsv_pool_wq workqueue_struct

struct lsv_pool_wq_work;

typedef void (*lsv_pool_wq_func_t)(struct lsv_pool_wq_work *w);

/*
 * Pool work is always a delayed_work underneath: queueing it with a zero delay
 * is the same as queue_work(), so the caller never picks between the two kinds.
 * The delay is a property of a single queueing, the period of the work itself.
 */
struct lsv_pool_wq_work {
	struct delayed_work dwork;
	lsv_pool_wq_func_t func;
	/* the queue it was last queued on: the periodic work re-arms itself there */
	struct lsv_pool_wq *wq;

	unsigned long period;
	bool stopped;
};

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

/* A work that runs once per queueing. Also revives a work that was cancelled. */
void lsv_pool_wq_init_work(struct lsv_pool_wq_work *w, lsv_pool_wq_func_t func);

/*
 * A work that queues itself again |period| jiffies after |func| returns, until
 * lsv_pool_wq_cancel_work(). |func| is one pass and knows nothing of the queue.
 * Runs never overlap: the next count starts when the previous run is over.
 */
void lsv_pool_wq_init_periodic_work(struct lsv_pool_wq_work *w, lsv_pool_wq_func_t func,
				    unsigned long period);

/*
 * Queues |w| to run after |delay| jiffies, 0 is at once. For a periodic work it
 * starts the cycle.
 *
 * false if |w| is already queued and has not started yet.
 */
bool lsv_pool_wq_queue_work(struct lsv_pool_wq *wq, struct lsv_pool_wq_work *w,
			    unsigned long delay);

/*
 * Stops a periodic work for good and waits for a run in progress: when it
 * returns |w| is neither queued nor running, so its owner can be freed.
 * Must not be called from |func| itself. Re-init |w| to use it again.
 */
void lsv_pool_wq_cancel_work(struct lsv_pool_wq_work *w);

#endif /* _LSV_POOL_WQ_H */
