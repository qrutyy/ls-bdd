// SPDX-License-Identifier: GPL-2.0-only

#include <linux/cpumask.h>
#include <linux/err.h>
#include <linux/slab.h>

#include "core/wq/wq.h"

struct lsv_wq *lsv_wq_create(enum lsv_wq_mode mode, const char *name)
{
	struct lsv_wq *wq;
	int rc;

	if (mode != LSV_WQ_MODE_PINNED && mode != LSV_WQ_MODE_HYBRID)
		return ERR_PTR(-EINVAL);

	wq = kzalloc(sizeof(*wq), GFP_KERNEL);
	if (!wq)
		return ERR_PTR(-ENOMEM);

	wq->mode = mode;

	if (mode == LSV_WQ_MODE_PINNED) {
		wq->shard = lsv_shard_wq_create(0, cpu_online_mask, "%s", name);
		if (IS_ERR(wq->shard)) {
			rc = PTR_ERR(wq->shard);
			goto free_wq;
		}
	}

	if (mode == LSV_WQ_MODE_POOL || mode == LSV_WQ_MODE_HYBRID) {
		wq->pool = lsv_pool_wq_create(0, cpu_online_mask, "%s", name);
		if (IS_ERR(wq->pool)) {
			rc = PTR_ERR(wq->pool);
			goto destroy_shard;
		}
	}

	return wq;

destroy_shard:
	lsv_shard_wq_destroy(wq->shard);
free_wq:
	kfree(wq);
	return ERR_PTR(rc);
}

void lsv_wq_destroy(struct lsv_wq *wq)
{
	if (IS_ERR_OR_NULL(wq))
		return;

	/* the pool may still queue shard jobs, so it goes first */
	lsv_pool_wq_destroy(wq->pool);
	lsv_shard_wq_destroy(wq->shard);
	kfree(wq);
}
