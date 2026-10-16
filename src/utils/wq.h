/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef _LSV_WQ_H
#define _LSV_WQ_H

#include <linux/cpumask.h>
#include <linux/kthread.h>

/*
 * Queues of kthread_workers, one worker per CPU of the mask a queue is created
 * with. A worker runs its jobs one at a time, in the order they were queued.
 *
 * On top of kthread_worker, every worker has a module-wide id that the DtA hash
 * table uses as the participant id ([DtA] assumes a fixed set of threads with
 * ids 0..n-1), and a worker can be found by its id and from inside itself.
 */

struct lsv_worker;
struct lsv_wq;
struct lsv_wq_ctx;

typedef void (*lsv_wq_func_t)(struct lsv_wq_ctx *ctx);

/* A job: embed it, and find the embedding structure with container_of(). */
struct lsv_wq_ctx {
	struct kthread_work work;
	lsv_wq_func_t func;
};

void lsv_wq_ctx_run(struct kthread_work *work);

static inline void lsv_wq_ctx_init(struct lsv_wq_ctx *ctx, lsv_wq_func_t func)
{
	kthread_init_work(&ctx->work, lsv_wq_ctx_run);
	ctx->func = func;
}

/*
 * Module-wide worker id: unique across every lsv_wq and never reused while the
 * module is loaded. All ids are below lsv_worker_id_limit() at the time of the
 * call, so a structure sized by that limit can index per-worker state by id.
 */
unsigned int lsv_worker_id(const struct lsv_worker *worker);
unsigned int lsv_worker_id_limit(void);
/*
 * The running worker of |id|, or NULL once its queue is destroyed. A caller
 * that queues work on it must keep that queue alive until the work has run.
 */
struct lsv_worker *lsv_worker_by_id(unsigned int id);
/*
 * Queues |ctx| on |worker|. A job already queued and not started yet stays
 * where it is and is not queued again. The job may free itself when it runs.
 */
void lsv_worker_add(struct lsv_worker *worker, struct lsv_wq_ctx *ctx);

/* |worker_cnt| <= 0 takes a worker for every online CPU of |cpumask|. */
__printf(3, 4) struct lsv_wq *
lsv_wq_create(int worker_cnt, const struct cpumask *cpumask, const char *namefmt, ...);
/* Runs the jobs already queued, then stops the workers. */
void lsv_wq_destroy(struct lsv_wq *wq);
/* The worker of |cpu|, or of some CPU of |cpumask| (NULL: the queue's own). */
struct lsv_worker *lsv_wq_map(struct lsv_wq *wq, const struct cpumask *cpumask,
			      unsigned int cpu);
/* Waits for every job queued on |wq| so far. */
void lsv_wq_flush(struct lsv_wq *wq);

/* The worker the caller runs on, NULL outside of a worker. */
struct lsv_worker *lsv_wq_current_worker(void);

#endif /* _LSV_WQ_H */
