// SPDX-License-Identifier: GPL-2.0-only

#include <linux/err.h>
#include <linux/printk.h>
#include <linux/stdarg.h>
#include <linux/workqueue.h>

#include "core/wq/pool_wq.h"

#define LSV_POOL_WQ_NAME_SIZE 32

__printf(3, 4) struct lsv_pool_wq *
lsv_pool_wq_create(int worker_cnt, const struct cpumask *cpumask, const char *namefmt, ...)
{
	char name[LSV_POOL_WQ_NAME_SIZE];
	struct lsv_pool_wq *wq;
	va_list args;
	int max_active;

	if (worker_cnt > 0) {
		max_active = worker_cnt;
	} else if (cpumask) {
		max_active = cpumask_weight_and(cpumask, cpu_online_mask);
		if (!max_active)
			return ERR_PTR(-EINVAL);
	} else {
		/* 0 is cmwq's own default */
		max_active = 0;
	}

	/* alloc_workqueue() is a macro and cannot take a va_list: name it here */
	va_start(args, namefmt);
	vsnprintf(name, sizeof(name), namefmt, args);
	va_end(args);

	wq = alloc_workqueue("%s", WQ_UNBOUND | WQ_MEM_RECLAIM, max_active, name);
	if (unlikely(!wq)) {
		pr_err("lsv: pool wq %s: failed to allocate\n", name);
		return ERR_PTR(-ENOMEM);
	}

	return wq;
}

void lsv_pool_wq_destroy(struct lsv_pool_wq *wq)
{
	if (IS_ERR_OR_NULL(wq))
		return;

	destroy_workqueue(wq);
}

void lsv_pool_wq_flush(struct lsv_pool_wq *wq)
{
	flush_workqueue(wq);
}

static void lsv_pool_wq_trampoline(struct work_struct *ws)
{
	struct lsv_pool_wq_work *w = container_of(to_delayed_work(ws), struct lsv_pool_wq_work, dwork);

	w->func(w);

	if (w->period && !READ_ONCE(w->stopped))
		queue_delayed_work(w->wq, &w->dwork, w->period);
}

void lsv_pool_wq_init_work(struct lsv_pool_wq_work *w, lsv_pool_wq_func_t func)
{
	INIT_DELAYED_WORK(&w->dwork, lsv_pool_wq_trampoline);
	w->func = func;
	w->wq = NULL;
	w->period = 0;
	w->stopped = false;
}

void lsv_pool_wq_init_periodic_work(struct lsv_pool_wq_work *w, lsv_pool_wq_func_t func,
				    unsigned long period)
{
	lsv_pool_wq_init_work(w, func);
	w->period = period;
}

bool lsv_pool_wq_queue_work(struct lsv_pool_wq *wq, struct lsv_pool_wq_work *w,
			    unsigned long delay)
{
	if (WARN_ON_ONCE(READ_ONCE(w->stopped)))
		return false;

	w->wq = wq;
	return queue_delayed_work(wq, &w->dwork, delay);
}

void lsv_pool_wq_cancel_work(struct lsv_pool_wq_work *w)
{
	WRITE_ONCE(w->stopped, true);
	cancel_delayed_work_sync(&w->dwork);
}
