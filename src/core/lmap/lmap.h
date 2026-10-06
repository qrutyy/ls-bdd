/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef LSV_CORE_LMAP_H
#define LSV_CORE_LMAP_H

#include <linux/bio.h>
#include <linux/rcupdate.h>
#include <linux/refcount.h>
#include <linux/types.h>
#include <linux/xarray.h>

#include "utils/ds_control.h"

/* Sectors reserved at the head of the backing device for metadata. */
#define LSV_META_SECTORS 32

#define LSV_CELL_SIZE_MIN 512
#define LSV_CELL_SIZE_MAX (1024 * 1024)
#define LSV_DS_NAME_LEN 8

struct lsv_de;

/*
 * |ref| counts exactly those cells (plus a writer that holds one in flight).
 * Freed after an RCU grace period, so a
 * reader that found it through a cell may keep using it under rcu_read_lock().
 */
struct lsv_pblk {
	u64 pba; /* physical cell number */
	refcount_t ref;

	struct lsv_de *de; /* dedup entry indexing this block, NULL if none */
	struct rcu_head rcu;
};

struct lsv_cell {
	u64 lba; /* logical cell number */
	struct lsv_pblk __rcu *pblk;
};

struct lsv_lmap {
	struct lsv_ds index;
	char ds_type[LSV_DS_NAME_LEN];

	u32 cell_size; /* bytes */
	u32 cell_sectors;
	u8 cell_sect_shift; /* log2(cell_sectors), a shift over sector numbers */

	u64 segment_size;

	atomic64_t next_pba; /* physical cell numbers, monotonic for now */
	u64 capacity_cells;

	/* Every live pblk by its pba: teardown walks it, GC will need it as the reverse map. */
	struct xarray pblks;

	struct bio_set bio_set;
};

s32 lsv_lmap_engine_init(void);
void lsv_lmap_engine_deinit(void);

s32 lsv_lmap_init(struct lsv_lmap *lmap, const char *ds_type, u32 cell_size, u64 segment_size,
		  sector_t backing_sectors);
void lsv_lmap_deinit(struct lsv_lmap *lmap);

struct lsv_pblk *lsv_pblk_alloc(struct lsv_lmap *lmap);
void lsv_pblk_put(struct lsv_lmap *lmap, struct lsv_pblk *pblk);

s32 lsv_lmap_bind(struct lsv_lmap *lmap, u64 lba, struct lsv_pblk *pblk);
bool lsv_lmap_lookup_pba(struct lsv_lmap *lmap, u64 lba, u64 *pba);

/* Where a sector lands inside the cell grid. */
struct lsv_lmap_pos {
	u64 lba; /* logical cell number */
	u32 offset; /* sector inside that cell */
	u32 sectors; /* sectors left until the end of the cell */
};

static inline void lsv_lmap_locate(const struct lsv_lmap *lmap, sector_t sector, struct lsv_lmap_pos *pos)
{
	pos->lba = sector >> lmap->cell_sect_shift;
	pos->offset = sector & (lmap->cell_sectors - 1);
	pos->sectors = lmap->cell_sectors - pos->offset;
}

/*
 * Logical addresses belong to the virtual device and start at zero; physical
 * ones are shifted by the metadata zone, hence the asymmetry with lsv_lmap_locate().
 */
static inline sector_t lsv_lmap_data_sector(const struct lsv_lmap *lmap, u64 pba)
{
	return LSV_META_SECTORS + (pba << lmap->cell_sect_shift);
}

#endif
