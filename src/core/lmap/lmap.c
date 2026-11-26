// SPDX-License-Identifier: GPL-2.0-only

#include <linux/blkdev.h>
#include <linux/bug.h>
#include <linux/err.h>
#include <linux/log2.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "core/dedup/dedup.h"
#include "core/lmap/lmap.h"

#define LSV_DEF_CELL_SIZE 4096
#define LSV_DEF_SEGMENT_SIZE (1 * 1024 * 1024)
#define LSV_DEF_BACK_CAP_SECTORS 0

/*
 * Module wide state of the mapping layer: the node caches every index
 * implementation allocates from, the cache the cells come from and the one the
 * physical block descriptors come from. Created once at module load and shared
 * by every device.
 */
struct lsv_lmap_engine {
	struct lsv_lmap_cache map_cache;
	struct kmem_cache *pblk_cachep;
};

static struct lsv_lmap_engine *g_lmap_engine;

s32 lsv_lmap_engine_init(void)
{
	struct lsv_lmap_engine *engine;
	struct lsv_lmap_cache *cache;

	engine = kzalloc(sizeof(*engine), GFP_KERNEL);
	if (!engine)
		return -ENOMEM;

	cache = &engine->map_cache;
	cache->cell_cachep = kmem_cache_create("lsv_cell", sizeof(struct lsv_cell), 0,
					       SLAB_HWCACHE_ALIGN, NULL);
	if (!cache->cell_cachep)
		goto free_engine;

	cache->entry_cache_mng = kzalloc(sizeof(*cache->entry_cache_mng), GFP_KERNEL);
	if (!cache->entry_cache_mng)
		goto destroy_cell_cache;

	engine->pblk_cachep = kmem_cache_create("lsv_pblk", sizeof(struct lsv_pblk), 0, 0, NULL);
	if (!engine->pblk_cachep)
		goto free_entry_cache_mng;

	g_lmap_engine = engine;

	return 0;

free_entry_cache_mng:
	kfree(cache->entry_cache_mng);
destroy_cell_cache:
	kmem_cache_destroy(cache->cell_cachep);
free_engine:
	kfree(engine);
	return -ENOMEM;
}

void lsv_lmap_engine_deinit(void)
{
	struct lsv_lmap_engine *engine = g_lmap_engine;
	struct lsv_lmap_cache *cache;
	struct lsv_cache_mng *mng;

	if (!engine)
		return;

	cache = &engine->map_cache;
	mng = cache->entry_cache_mng;

	/* The index node caches are created lazily by the first device using them. */
	kmem_cache_destroy(mng->ht_cache);
	kmem_cache_destroy(mng->sl_cache);
	kmem_cache_destroy(mng->bt_cache);
	kmem_cache_destroy(mng->rb_cache);
	kfree(mng);

	kmem_cache_destroy(cache->cell_cachep);
	/* Waits for the pblk_pba_map still queued by kfree_rcu(). */
	kmem_cache_destroy(engine->pblk_cachep);

	g_lmap_engine = NULL;
	kfree(engine);
}

static void lsv_lmap_validate_params(struct lsv_lmap *lmap)
{
	if (!lmap->params.cell_size)
		lmap->params.cell_size = LSV_DEF_CELL_SIZE;

	/* No default for the layout: 0 is LSV_SPACE_LINEAR, a valid choice of its own. */

	if (!lmap->params.segment_size)
		lmap->params.segment_size = LSV_DEF_SEGMENT_SIZE;

	if (!lmap->params.back_cap_sectors)
		lmap->params.back_cap_sectors = LSV_DEF_BACK_CAP_SECTORS;

}

static s32 lsv_lmap_setup_geometry(struct lsv_lmap *lmap)
{
	u32 cell_size;
	sector_t backing_sectors;

	lsv_lmap_validate_params(lmap);

	cell_size = lmap->params.cell_size;
	backing_sectors = lmap->params.back_cap_sectors;

	if (cell_size < LSV_CELL_SIZE_MIN || cell_size > LSV_CELL_SIZE_MAX)
		return -EINVAL;

	if (!is_power_of_2(cell_size) || (cell_size & (SECTOR_SIZE - 1)))
		return -EINVAL;

	lmap->cell_size = cell_size;
	lmap->cell_sectors = cell_size >> SECTOR_SHIFT;
	lmap->cell_sect_shift = ilog2(lmap->cell_sectors);

	if (backing_sectors <= LSV_META_SECTORS)
		return -ENOSPC;

	lmap->capacity_cells = (backing_sectors - LSV_META_SECTORS) >> lmap->cell_sect_shift;
	if (!lmap->capacity_cells)
		return -ENOSPC;

	lmap->segment_size = lmap->params.segment_size;

	return 0;
}

s32 lsv_lmap_init(struct lsv_lmap *lmap)
{
	s32 rc;

	if (strscpy(lmap->ds_type, lmap->params.index_ds, sizeof(lmap->ds_type)) < 0)
		return -ENAMETOOLONG;

	rc = lsv_lmap_setup_geometry(lmap);
	if (rc)
		return rc;

	xa_init(&lmap->pblk_pba_map);

	/* Only the linear layout tracks free cells one by one. */
	if (lsv_lmap_is_linear(lmap)) {
		rc = lfbm_init(&lmap->pba_bitmap, lmap->capacity_cells);
		if (rc)
			return rc;
	}

	rc = lsv_ds_init(&lmap->index, lmap->ds_type, g_lmap_engine->map_cache.entry_cache_mng, lmap->capacity_cells);
	if (rc)
		goto destroy_bitmap;

	return 0;

destroy_bitmap:
	lfbm_destroy(&lmap->pba_bitmap);
	return rc;
}

/*
 * The device is quiesced by now, so nothing races with this. Every pblk still
 * referenced by a cell is in |pblk_pba_map|; they are freed directly, without going
 * through lsv_pblk_put(): the dedup entries pointing at them are already gone.
 */
void lsv_lmap_deinit(struct lsv_lmap *lmap)
{
	struct lsv_pblk *pblk;
	unsigned long pba;

	lsv_ds_free(&lmap->index, &g_lmap_engine->map_cache);

	xa_for_each(&lmap->pblk_pba_map, pba, pblk)
		kmem_cache_free(g_lmap_engine->pblk_cachep, pblk);
	xa_destroy(&lmap->pblk_pba_map);
	lfbm_destroy(&lmap->pba_bitmap);
}

/*
 * The lowest free cell: the linear layout fills the gaps the dead ones left.
 * Bounded by the capacity, so a full device is -ENOSPC from the bitmap itself.
 * It neither locks nor allocates, which is what the write path wants.
 */
static s64 lsv_lmap_linear_next_pba_get(struct lsv_lmap *lmap)
{
	return lfbm_find_and_set_bit(&lmap->pba_bitmap);
}

static s64 lsv_lmap_segmented_next_pba_get(struct lsv_lmap *lmap)
{
	/* TODO(maksim): add segment model pba allocation */
	return -EOPNOTSUPP;
}

static s32 lsv_lmap_alloc_pba(struct lsv_lmap *lmap, u64 *pba)
{
	s64 rc;

	if (lsv_lmap_is_linear(lmap))
		rc = lsv_lmap_linear_next_pba_get(lmap);
	else
		rc = lsv_lmap_segmented_next_pba_get(lmap);

	if (rc < 0)
		return rc;

	*pba = rc;

	return 0;
}

/* Gives the cell back for reuse. Only the linear layout tracks free cells one by one. */
static void lsv_lmap_free_pba(struct lsv_lmap *lmap, u64 pba)
{
	if (lsv_lmap_is_linear(lmap))
		WARN_ON_ONCE(!lfbm_test_and_clear_bit(&lmap->pba_bitmap, pba));
}

/*
 * A fresh physical cell, returned with one reference held by the caller. That
 * reference is what lsv_lmap_bind() later hands over to the cell.
 */
struct lsv_pblk *lsv_pblk_alloc(struct lsv_lmap *lmap)
{
	struct lsv_pblk *pblk;
	s32 rc;

	pblk = kmem_cache_alloc(g_lmap_engine->pblk_cachep, GFP_NOIO);
	if (!pblk)
		return ERR_PTR(-ENOMEM);

	refcount_set(&pblk->ref, 1);
	pblk->de = NULL;

	rc = lsv_lmap_alloc_pba(lmap, &pblk->pba);
	if (rc)
		goto free_pblk;

	rc = xa_err(xa_store(&lmap->pblk_pba_map, pblk->pba, pblk, GFP_NOIO));
	if (rc)
		goto free_pba;

	return pblk;

free_pba:
	lsv_lmap_free_pba(lmap, pblk->pba);
free_pblk:
	kmem_cache_free(g_lmap_engine->pblk_cachep, pblk);
	return ERR_PTR(rc);
}

void lsv_pblk_put(struct lsv_lmap *lmap, struct lsv_pblk *pblk)
{
	if (!refcount_dec_and_test(&pblk->ref))
		return;

	if (pblk->de)
		lsv_dedup_forget(pblk->de);

	/*
	 * TODO(gc): this is the single place a physical cell dies. Once segments
	 * exist it has to be reported here, along the lines of
	 *	atomic_inc(&lmap->segs[pblk->pba / cells_per_segment].dead);
	 */

	xa_erase(&lmap->pblk_pba_map, pblk->pba);
	/*
	 * TODO: the cell may be handed out again at once, while a reader that got
	 * this pba from lsv_lmap_lookup_pba() or a dedup verify still has its read
	 * in flight, and then reads the new data. Needs a count of reads in flight
	 * (per pblk here, per segment in the segmented layout), see the GC design.
	 */
	lsv_lmap_free_pba(lmap, pblk->pba);
	kfree_rcu(pblk, rcu);
}

/* Points |lba| at |pblk|, consuming the reference the caller holds on it.  */
s32 lsv_lmap_bind(struct lsv_lmap *lmap, u64 lba, struct lsv_pblk *pblk)
{
	struct lsv_lmap_cache *cache = &g_lmap_engine->map_cache;
	struct lsv_pblk *old;
	struct lsv_cell *cell;
	s32 rc;

	cell = lsv_ds_lookup(&lmap->index, lba);
	if (cell) {
		/*
		 * An exchange rather than a load and a store: two writers of the
		 * same lba must each drop exactly the pblk they replaced.
		 */
		old = unrcu_pointer(xchg(&cell->pblk, RCU_INITIALIZER(pblk)));
		lsv_pblk_put(lmap, old);

		return 0;
	}

	cell = kmem_cache_alloc(cache->cell_cachep, GFP_NOIO);
	if (!cell)
		return -ENOMEM;

	cell->lba = lba;
	RCU_INIT_POINTER(cell->pblk, pblk);

	rc = lsv_ds_insert(&lmap->index, lba, cell, cache);
	if (rc) {
		kmem_cache_free(cache->cell_cachep, cell);
		return rc;
	}

	return 0;
}

/* False for an lba that was never written. */
bool lsv_lmap_lookup_pba(struct lsv_lmap *lmap, u64 lba, u64 *pba)
{
	struct lsv_cell *cell;
	bool found = false;

	/* The pblk may die under a concurrent overwrite; RCU keeps it readable. */
	rcu_read_lock();

	cell = lsv_ds_lookup(&lmap->index, lba);
	if (cell) {
		*pba = READ_ONCE(rcu_dereference(cell->pblk)->pba);
		found = true;
	}

	rcu_read_unlock();

	return found;
}
