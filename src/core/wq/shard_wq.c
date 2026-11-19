// SPDX-License-Identifier: GPL-2.0-only

#include <linux/err.h>
#include <linux/overflow.h>
#include <linux/preempt.h>
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/topology.h>
#include <linux/vmalloc.h>
#include <linux/xarray.h>

#include "shard_wq.h"

#define LSV_SHARD_WQ_NAME_SIZE	32

/*
 * The kthread_worker is embedded rather than made by kthread_create_worker_*(),
 * so that the worker a thread runs is a container_of() away from kthread_data().
 */
struct lsv_shard_wq_worker {
	struct kthread_worker kw;
	struct lsv_shard_wq *wq;
	/* NULL until the thread is created; kw.task is set by the thread itself */
	struct task_struct *task;
	unsigned int cpu_num;
	unsigned int id;
	bool registered;
};

struct lsv_shard_wq {
	char name[LSV_SHARD_WQ_NAME_SIZE];
	cpumask_var_t cpumask;

	int cpu_cnt;
	struct lsv_shard_wq_worker **worker_map;

	int worker_cnt;
	struct lsv_shard_wq_worker workers[] __counted_by(worker_cnt);
};

/* every running worker by its module-wide id */
static DEFINE_XARRAY(g_lsv_shard_wq_workers);

/* Zero-initialized storage is a valid atomic_t holding 0. */
static atomic_t g_lsv_shard_wq_worker_id_next;

void lsv_shard_wq_work_run(struct kthread_work *kwork)
{
	struct lsv_shard_wq_work *work = container_of(kwork, struct lsv_shard_wq_work, work);

	work->func(work);
}

/*
 * kthread_worker_fn() under a name of our own, so that a thread of ours is told
 * apart from any other kthread_worker by its thread function alone.
 */
static int lsv_shard_wq_worker_fn(void *kw)
{
	return kthread_worker_fn(kw);
}

static int worker_run(struct lsv_shard_wq_worker *worker, const char *name, unsigned int cpu)
{
	struct task_struct *task;

	kthread_init_worker(&worker->kw);

	task = kthread_create_on_node(lsv_shard_wq_worker_fn, &worker->kw, cpu_to_node(cpu),
				      "lsv-%s/%u", name, cpu);
	if (IS_ERR(task))
		return PTR_ERR(task);

	worker->task = task;
	worker->cpu_num = cpu;

	kthread_bind(task, cpu);
	wake_up_process(task);

	return 0;
}

/* A stopping kthread_worker drops what is still queued: run it first. */
static void worker_stop(struct lsv_shard_wq_worker *worker)
{
	if (!worker->task)
		return;

	kthread_flush_worker(&worker->kw);
	kthread_stop(worker->task);
}

unsigned int lsv_shard_wq_worker_id(const struct lsv_shard_wq_worker *worker)
{
	return worker->id;
}

unsigned int lsv_shard_wq_worker_id_limit(void)
{
	return atomic_read(&g_lsv_shard_wq_worker_id_next);
}

struct lsv_shard_wq_worker *lsv_shard_wq_worker_by_id(unsigned int id)
{
	return xa_load(&g_lsv_shard_wq_workers, id);
}

bool lsv_shard_wq_queue_work_by_id(unsigned int id, struct lsv_shard_wq_work *work)
{
	struct lsv_shard_wq_worker *worker = lsv_shard_wq_worker_by_id(id);

	if (!worker)
		return false;

	return kthread_queue_work(&worker->kw, &work->work);
}

bool lsv_shard_wq_queue_work_current(struct lsv_shard_wq_work *work)
{
	struct lsv_shard_wq_worker *worker = lsv_shard_wq_current_worker();

	if (!worker)
		return false;

	return kthread_queue_work(&worker->kw, &work->work);
}


static struct lsv_shard_wq *wq_create(const struct cpumask *cpumask, const char *name)
{
	struct lsv_shard_wq_worker *worker;
	int worker_cnt;
	unsigned int cpu;
	struct lsv_shard_wq *wq;
	int i, rc;

	worker_cnt = cpumask_weight(cpumask);

	wq = vzalloc(struct_size(wq, workers, worker_cnt));
	if (unlikely(!wq))
		return ERR_PTR(-ENOMEM);

	wq->worker_cnt = worker_cnt;
	strscpy(wq->name, name, sizeof(wq->name));

	if (!zalloc_cpumask_var(&wq->cpumask, GFP_KERNEL)) {
		rc = -ENOMEM;
		goto out_err;
	}
	cpumask_copy(wq->cpumask, cpumask);

	wq->cpu_cnt = cpumask_last(cpumask) + 1;
	wq->worker_map = vcalloc(wq->cpu_cnt, sizeof(*wq->worker_map));
	if (unlikely(!wq->worker_map)) {
		rc = -ENOMEM;
		goto out_err;
	}

	i = 0;
	for_each_cpu(cpu, cpumask) {
		worker = &wq->workers[i++];
		worker->wq = wq;
		wq->worker_map[cpu] = worker;

		rc = worker_run(worker, wq->name, cpu);
		if (unlikely(rc)) {
			pr_err("lsv: wq %s: failed to run a worker on cpu %u, rc=%d\n",
			       wq->name, cpu, rc);
			goto out_err;
		}

		/* Ids are never reused: queues are created at module setup. */
		worker->id = atomic_fetch_inc(&g_lsv_shard_wq_worker_id_next);
		rc = xa_insert(&g_lsv_shard_wq_workers, worker->id, worker, GFP_KERNEL);
		if (unlikely(rc)) {
			pr_err("lsv: wq %s: failed to register worker id=%u, rc=%d\n",
			       wq->name, worker->id, rc);
			goto out_err;
		}
		worker->registered = true;
	}

	/* TODO: support CPU hotplug */

	return wq;

out_err:
	lsv_shard_wq_destroy(wq);
	return ERR_PTR(rc);
}

struct lsv_shard_wq *
lsv_shard_wq_create(int worker_cnt, const struct cpumask *cpumask, const char *namefmt, ...)
{
	char name[LSV_SHARD_WQ_NAME_SIZE];
	cpumask_var_t cur_cpumask;
	struct lsv_shard_wq *wq;
	int max_active;
	va_list args;

	if (!zalloc_cpumask_var(&cur_cpumask, GFP_KERNEL))
		return ERR_PTR(-ENOMEM);

	if (!cpumask_and(cur_cpumask, cpumask, cpu_online_mask)) {
		free_cpumask_var(cur_cpumask);
		return ERR_PTR(-EINVAL);
	}

	max_active = cpumask_weight(cur_cpumask);

	if (worker_cnt > 0 && max_active > worker_cnt)
		while (max_active-- > worker_cnt)
			cpumask_clear_cpu(cpumask_last(cur_cpumask), cur_cpumask);

	va_start(args, namefmt);
	vsnprintf(name, sizeof(name), namefmt, args);
	va_end(args);

	wq = wq_create(cur_cpumask, name);

	free_cpumask_var(cur_cpumask);
	return wq;
}

void lsv_shard_wq_destroy(struct lsv_shard_wq *wq)
{
	struct lsv_shard_wq_worker *worker;
	int i;

	if (IS_ERR_OR_NULL(wq))
		return;

	for (i = 0; i < wq->worker_cnt; i++) {
		worker = &wq->workers[i];

		/* a worker that never ran has no thread and no id */
		if (worker->registered)
			xa_erase(&g_lsv_shard_wq_workers, worker->id);
		worker_stop(worker);
	}

	vfree(wq->worker_map);
	free_cpumask_var(wq->cpumask);
	vfree(wq);
}

struct lsv_shard_wq_worker *lsv_shard_wq_map(struct lsv_shard_wq *wq, const struct cpumask *cpumask,
			      unsigned int cpu)
{
	if (!cpumask)
		cpumask = wq->cpumask;

	if (cpu >= wq->cpu_cnt || !cpumask_test_cpu(cpu, cpumask))
		cpu = cpumask_nth(cpu % cpumask_weight(cpumask), cpumask);

	return wq->worker_map[cpu];
}

void lsv_shard_wq_flush(struct lsv_shard_wq *wq)
{
	int i;

	for (i = 0; i < wq->worker_cnt; i++)
		if (wq->workers[i].task)
			kthread_flush_worker(&wq->workers[i].kw);
}

struct lsv_shard_wq_worker *lsv_shard_wq_current_worker(void)
{
	if (!in_task() || kthread_func(current) != lsv_shard_wq_worker_fn)
		return NULL;

	return container_of(kthread_data(current), struct lsv_shard_wq_worker, kw);
}
