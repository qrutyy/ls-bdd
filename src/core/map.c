// SPDX-License-Identifier: GPL-2.0-only

#include <linux/blkdev.h>
#include <linux/log2.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "core/map.h"

/*
 * Module wide state of the mapping layer: the node caches every index
 * implementation allocates from and the cache the cells come from. Created once
 * at module load and shared by every device.
 */
struct lsv_lmap_engine {
	struct lsv_lmap_cache map_cache;
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

	g_lmap_engine = engine;

	return 0;

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

	g_lmap_engine = NULL;
	kfree(engine);
}

static s32 lsv_lmap_setup_geometry(struct lsv_lmap *map, u32 cell_size, sector_t backing_sectors)
{
	if (cell_size < LSV_CELL_SIZE_MIN || cell_size > LSV_CELL_SIZE_MAX)
		return -EINVAL;

	if (!is_power_of_2(cell_size) || (cell_size & (SECTOR_SIZE - 1)))
		return -EINVAL;

	map->cell_size = cell_size;
	map->cell_sectors = cell_size >> SECTOR_SHIFT;
	map->cell_sect_shift = ilog2(map->cell_sectors);

	if (backing_sectors <= LSV_META_SECTORS)
		return -ENOSPC;

	map->capacity_cells = (backing_sectors - LSV_META_SECTORS) >> map->cell_sect_shift;
	if (!map->capacity_cells)
		return -ENOSPC;

	atomic64_set(&map->next_pba, 0);

	return 0;
}

s32 lsv_lmap_init(struct lsv_lmap *map, const char *ds_type, u32 cell_size, u64 segment_size,
		 sector_t backing_sectors)
{
	s32 rc;

	if (strscpy(map->ds_type, ds_type, sizeof(map->ds_type)) < 0)
		return -ENAMETOOLONG;

	rc = lsv_lmap_setup_geometry(map, cell_size, backing_sectors);
	if (rc)
		return rc;

	map->segment_size = segment_size;

	rc = bioset_init(&map->bio_set, BIO_POOL_SIZE, 0, BIOSET_NEED_BVECS);
	if (rc)
		return rc;

	rc = lsv_ds_init(&map->index, map->ds_type, g_lmap_engine->map_cache.entry_cache_mng);
	if (rc)
		goto bioset_err;

	return 0;

bioset_err:
	bioset_exit(&map->bio_set);
	return rc;
}

void lsv_lmap_deinit(struct lsv_lmap *map)
{
	lsv_ds_free(&map->index, &g_lmap_engine->map_cache);
	bioset_exit(&map->bio_set);
}

struct lsv_cell *lsv_lmap_lookup(struct lsv_lmap *map, u64 lba)
{
	return lsv_ds_lookup(&map->index, lba);
}

static s32 lsv_lmap_alloc_pba(struct lsv_lmap *map, u64 *pba)
{
	s64 allocated;

	allocated = atomic64_fetch_add(1, &map->next_pba);
	if (allocated < 0 || (u64)allocated >= map->capacity_cells) {
		atomic64_dec(&map->next_pba);
		return -ENOSPC;
	}

	*pba = allocated;

	return 0;
}

/*
 * Redirect-on-write: every write lands on a freshly allocated physical cell and
 * the mapping entry is repointed at it.
 *
 * The entry is updated in place rather than removed and reinserted. That keeps
 * the write path down to one lookup plus one store, and it removes the window in
 * which a concurrent read of the same lba would find no mapping at all and be
 * served zeroes for data that does exist.
 */
struct lsv_cell *lsv_lmap_process(struct lsv_lmap *map, u64 lba)
{
	struct lsv_lmap_cache *cache = &g_lmap_engine->map_cache;
	struct lsv_cell *cell;
	u64 allocated;
	s32 rc;

	rc = lsv_lmap_alloc_pba(map, &allocated);
	if (rc)
		return ERR_PTR(rc);

	cell = lsv_ds_lookup(&map->index, lba);
	if (cell) {
		/*
		 * TODO(gc): READ_ONCE(cell->pba) is the physical cell that just
		 * died. Once segments exist this is the single place that has to
		 * report it, along the lines of
		 *	atomic_inc(&map->segs[old / cells_per_segment].dead);
		 * Nothing is released here on purpose: reclaiming physical space
		 * is the collector's job, the write path only repoints.
		 */
		WRITE_ONCE(cell->pblk->pba, allocated);

		return cell;
	}

	cell = kmem_cache_alloc(cache->cell_cachep, GFP_NOIO);
	if (!cell)
		return ERR_PTR(-ENOMEM);

	cell->lba = lba;
	cell->pblk.pba = allocated;

	rc = lsv_ds_insert(&map->index, lba, cell, cache);
	if (rc) {
		kmem_cache_free(cache->cell_cachep, cell);
		return ERR_PTR(rc);
	}

	return cell;
}

void lsv_pblk_put(struct lsv_pblk *pblk) {
	if (!refcount_dec_and_test(&pblk->ref))
		return;

	if (pblk->de)
		lsv_dedup_forget(pblk->de);

	/* TODO(?) mark slot as dead in the segment
	 * lsv_seg_invalidate(map, pblk->pba);     dead++
	 */

	/* TODO(?) kfree_rcu(pblk); */
}

s32 *lsv_lmap_repoint(struct lsv_lmap *map, u64 lba, struct lsv_pblk *pblk)
{
	struct lsv_lmap_cache *cache = &g_lmap_engine->map_cache;
	struct lsv_cell *cell;
	u64 allocated;
	s32 rc;

	cell = lsv_ds_lookup(&map->index, lba);
	      if (cell) {
		      old = xchg(&cell->pblk, pblk);

		      lsv_pblk_put(map, old); /* ref-- ; на 0 — dead++ в сегменте, снять de */
		      return 0;
	      }

	/* TODO(qrutyy): move in sep function (like init_cell) */
	cell = kmem_cache_alloc(cache->cell_cachep, GFP_NOIO);
	if (!cell)
		return ERR_PTR(-ENOMEM);

	cell->lba = lba;
	cell->pblk = pblk;

	rc = lsv_ds_insert(&map->index, lba, cell, cache);
	if (rc) {
		kmem_cache_free(cache->cell_cachep, cell);
		return ERR_PTR(rc);
	}

	return 0;
}
