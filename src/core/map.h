/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef LSV_CORE_MAP_H
#define LSV_CORE_MAP_H

#include <linux/bio.h>
#include <linux/types.h>

#include "utils/ds_control.h"

/* Sectors reserved at the head of the backing device for metadata. */
#define LSV_META_SECTORS 32

#define LSV_CELL_SIZE_MIN 512
#define LSV_CELL_SIZE_MAX (1024 * 1024)
#define LSV_DS_NAME_LEN 8

/*
 * One mapping entry. The index is keyed by the logical cell number, so the lba
 * below is redundant for lookups; it is kept because the reverse direction
 * (segment summary, GC) will need it.
 */
struct lsv_cell {
	u64 lba; /* logical cell number */
	u64 pba; /* physical cell number */
};

/* struct lsv_map_cache lives in utils/ds_control.h and is owned by the module. */

struct lsv_map {
	struct lsv_ds index;
	char ds_type[LSV_DS_NAME_LEN];

	u32 cell_size; /* bytes */
	u32 cell_sectors;
	u8 cell_sect_shift; /* log2(cell_sectors), a shift over sector numbers */

	u64 segment_size;

	atomic64_t next_pba; /* physical cell numbers, monotonic for now */
	u64 capacity_cells;

	struct bio_set bio_set;
	struct lsv_map_cache *cache; /* module owned */
};

s32 lsv_map_cache_alloc(struct lsv_map_cache *cache);
void lsv_map_cache_free(struct lsv_map_cache *cache);

s32 lsv_map_init(struct lsv_map *map, struct lsv_map_cache *cache, const char *ds_type, u32 cell_size, u64 segment_size,
		 sector_t backing_sectors);
void lsv_map_deinit(struct lsv_map *map);

struct lsv_cell *lsv_map_lookup(struct lsv_map *map, u64 lba);
s32 lsv_map_remap(struct lsv_map *map, u64 lba, u64 *pba);

/* Where a sector lands inside the cell grid. */
struct lsv_map_pos {
	u64 lba; /* logical cell number */
	u32 offset; /* sector inside that cell */
	u32 sectors; /* sectors left until the end of the cell */
};

static inline void lsv_map_locate(const struct lsv_map *map, sector_t sector, struct lsv_map_pos *pos)
{
	pos->lba = sector >> map->cell_sect_shift;
	pos->offset = sector & (map->cell_sectors - 1);
	pos->sectors = map->cell_sectors - pos->offset;
}

/*
 * Logical addresses belong to the virtual device and start at zero; physical
 * ones are shifted by the metadata zone, hence the asymmetry with lsv_map_locate().
 */
static inline sector_t lsv_map_data_sector(const struct lsv_map *map, u64 pba)
{
	return LSV_META_SECTORS + (pba << map->cell_sect_shift);
}

#endif
