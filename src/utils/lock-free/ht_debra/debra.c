#include <linux/atomic.h>
#include <linux/minmax.h>
#include <linux/overflow.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <kunit/static_stub.h>

#include "debra.h"
#include "utils/wq.h"

static void debra_rotate_and_reclaim(struct lsv_debra_state *debra,
				     struct lsv_debra_local_rec *loc);

void debra_leave_qstate(struct lsv_debra_state *debra, int tid)
{
	// get the current epoch and compare it with previous announcement
	// if they differ then should rotate and reclaim, update own announcement
	u64 global_epoch = atomic64_read(&debra->epoch);
	struct lsv_debra_global_rec *worker_global_rec = debra->global + tid;
	struct lsv_debra_local_rec *worker_local_rec = debra->limbo->rec + tid;
	u64 worker_epoch = worker_global_rec->state;

	if (global_epoch != lsv_debra_state_epoch(worker_epoch))
	{
		worker_local_rec->op_cnt = 0;
		worker_local_rec->next_to_check = 0;
		debra_rotate_and_reclaim(debra, worker_local_rec);
	}

	// incremental scan of all other announcements --- if all of them are quiescent
	// or have already observed this epoch can try to advance

	if (++worker_local_rec->op_cnt >= LSV_CHECKS_THRESHOLD)
	{
		worker_local_rec->op_cnt = 0;
		int other = worker_local_rec->next_to_check % debra->nr_threads;
		u64 other_epoch = debra->global[other].state;
		if ((global_epoch == lsv_debra_state_epoch(other_epoch)) || !lsv_debra_state_active(other_epoch))
		{
			++worker_local_rec->next_to_check;
			if (worker_local_rec->next_to_check >= debra->nr_threads && worker_local_rec->next_to_check >= LSV_INCR_THRESHOLD)
				ATOMIC_LCAS(&debra->epoch, global_epoch, global_epoch + 1);
		}
	}

	// need a full barrier after announcement to fix the global epoch before grabbing any pointers
	debra->global[tid].state = lsv_debra_state_make(global_epoch);
	smp_mb();
}


void debra_enter_qstate(struct lsv_debra_state *debra, int tid)
{
	struct lsv_debra_global_rec *worker_global_rec = debra->global + tid;
	smp_store_release(&worker_global_rec->state, lsv_debra_state_epoch(worker_global_rec->state) << 1);
}


void debra_retire(struct lsv_debra_state *debra, int tid, struct lsv_lf_list_node *node)
{
	// get proper local limbo rec
	struct lsv_debra_local_rec *loc = debra->limbo->rec + tid;
	struct lsv_debra_limbo *limbo = loc->limbo + loc->cur;

	// add to this bag a node which is about to be retired
	node->removed_link = limbo->head;
	limbo->head = node;
	limbo->cnt++;
	loc->retire_cnt++;

	// call free if limbo is filled
}


u32 debra_free_limbo(struct lsv_debra_state *debra, struct lsv_lf_list_node *node)
{
	struct lsv_lf_list_node *next;
	u32 freed = 0;

	for (; node; node = next) {
		next = node->removed_link;
		debra->ops->node_dealloc(node, debra->ctx);
		freed++;
	}

	return freed;
}


static void debra_rotate_and_reclaim(struct lsv_debra_state *debra, struct lsv_debra_local_rec *loc)
{
	struct lsv_debra_limbo *limbo;
	u32 freed;

	loc->cur = (loc->cur + 1) % 3;
	limbo = loc->limbo + loc->cur;

	freed = limbo->cnt;
	loc->retire_cnt -= freed;

	debra_free_limbo(debra, limbo->head);
	limbo->head = NULL;
	limbo->cnt = 0;
}


void debra_retire_span(struct lsv_debra_state *debra, int tid,
		       struct lsv_lf_list_node *first, struct lsv_lf_list_node *last)
{
	struct lsv_lf_list_node *node;
	struct lsv_lf_list_node *next;

	for (node = lsv_ht_strip_mark(first); node != last; node = next) {
		next = lsv_ht_strip_mark(READ_ONCE(node->next));
		debra_retire(debra, tid, node);
	}
}


int debra_limbo_alloc(struct lsv_debra_state *debra)
{
	struct lsv_debra_limbo_state *limbo;

	limbo = kvzalloc(struct_size(limbo, rec, debra->nr_threads), GFP_KERNEL);
	if (!limbo)
		return -ENOMEM;

	limbo->debra = debra;
	debra->limbo = limbo;
	return 0;
}


void debra_limbo_destroy(struct lsv_debra_state *debra)
{
	struct lsv_debra_limbo_state *limbo = debra->limbo;
	unsigned int i, j;

	for (i = 0; i < debra->nr_threads; i++)
		for (j = 0; j < LSV_DEBRA_NR_EPOCHS; j++)
			debra_free_limbo(debra, limbo->rec[i].limbo[j].head);

	kvfree(limbo);
	debra->limbo = NULL;
}


size_t debra_limbo_mem_usage(const struct lsv_debra_state *debra)
{
	return struct_size(debra->limbo, rec, debra->nr_threads);
}


struct lsv_debra_state *debra_alloc(struct lsv_ht *ht)
{
	struct lsv_debra_state *debra;

	debra = kzalloc_obj(*debra);
	if (!debra)
		return NULL;

	debra->nr_threads = max_t(unsigned int, lsv_worker_id_limit(), 1);
	debra->ht = ht;
	debra->ops = ht->ops;
	debra->ctx = ht->ctx;

	debra->global = kvzalloc_objs(*debra->global, debra->nr_threads);
	if (!debra->global)
		goto err_free;

	if (debra_limbo_alloc(debra))
		goto err_free;

	return debra;

err_free:
	kvfree(debra->global);
	kfree(debra);
	return NULL;
}


void debra_destroy(struct lsv_debra_state *debra)
{
	debra_limbo_destroy(debra);
	kvfree(debra->global);
	kfree(debra);
}


int debra_tid(const struct lsv_ht *ht)
{
	struct lsv_worker *worker = lsv_wq_current_worker();
	unsigned int id;

	if (WARN_ON_ONCE(!worker))
		return -1;

	id = lsv_worker_id(worker);
	if (unlikely(id >= ht->debra->nr_threads)) {
		pr_warn_ratelimited("lsv_ht: worker %u is not a participant of a table sized for %u\n",
				    id, ht->debra->nr_threads);
		return -1;
	}

	return id;
}


u64 debra_epoch(const struct lsv_debra_state *debra)
{
	return atomic64_read(&debra->epoch);
}


size_t debra_mem_usage(const struct lsv_debra_state *debra)
{
	return sizeof(*debra) + debra->nr_threads * sizeof(*debra->global) +
	       debra_limbo_mem_usage(debra);
}

#if IS_ENABLED(CONFIG_KUNIT)
void (*g_lsv_ht_kunit_cell_check)(struct lsv_lf_list_node *cell);

void ht_kunit_cell_reached(struct lsv_lf_list_node *cell)
{
	void (*check)(struct lsv_lf_list_node *cell) = READ_ONCE(g_lsv_ht_kunit_cell_check);

	if (check)
		check(cell);
	KUNIT_STATIC_STUB_REDIRECT(ht_kunit_cell_reached, cell);
}

bool ht_debra_is_active(const struct lsv_ht *ht, unsigned int tid)
{
	return lsv_debra_state_active(READ_ONCE(ht->debra->global[tid].state));
}

u32 ht_debra_limbo_cnt(const struct lsv_ht *ht, unsigned int tid)
{
	return READ_ONCE(ht->debra->limbo->rec[tid].retire_cnt);
}

u32 ht_debra_limbo_total(const struct lsv_ht *ht)
{
	u32 total = 0;
	unsigned int i;

	for (i = 0; i < ht->debra->nr_threads; i++)
		total += ht_debra_limbo_cnt(ht, i);

	return total;
}

bool ht_kunit_reclaim_requests_pending(const struct lsv_ht *ht)
{
	return false;
}
#endif
