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

bool lsv_pool_wq_queue_work(struct lsv_pool_wq *wq, struct lsv_pool_wq_work *work)
{
	return queue_work(wq, work);
}
