/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef LSV_CORE_LMAP_H
#define LSV_CORE_LMAP_H

#include <linux/bio.h>
#include <linux/idr.h>
#include <linux/rcupdate.h>
#include <linux/refcount.h>
#include <linux/types.h>
#include <linux/xarray.h>
#include "linux/spinlock.h"
#include "linux/atomic.h"

#include "utils/ds_control.h"

/* Sectors reserved at the head of the backing device for metadata. */
#define LSV_META_SECTORS 32

#define LSV_CELL_SIZE_MIN 512
#define LSV_CELL_SIZE_MAX (1024 * 1024)
#define LSV_DS_NAME_LEN 8

/* Segment model constants. */
#define LSV_SEGMENT_MIN_CELLS 256
#define LSV_SEGMENT_DEFAULT_SIZE (16 * 1024 * 1024)
#define LSV_GC_RESERVE_SEGS 1
#define LSV_REFS_HIST_BUCKETS 5

struct lsv_de;

struct lsv_segment;

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
	struct lsv_segment *segment; /* segment this block belongs to */
};

struct lsv_cell {
	u64 lba; /* logical cell number */
	struct lsv_pblk __rcu *pblk;
};

/* How physical cells are laid out on the backing device. */
enum lsv_space_layout {
	LSV_SPACE_LINEAR,
	LSV_SPACE_SEGMENTED,
};

enum lsv_segment_state {
	LSV_SEGMENT_FREE = 0,
	LSV_SEGMENT_OPEN,
	LSV_SEGMENT_SEALING,
	LSV_SEGMENT_CLOSED,
	LSV_SEGMENT_VICTIM,
};

enum lsv_segment_stream {
	LSV_SEGMENT_USER = 0,
	LSV_SEGMENT_STREAM_GC,
	LSV_SEGMENT_STREAM_NR,
};

/*
 * A fixed-size region of physical storage that is allocated and reclaimed
 * as a unit. Its lifecycle and counters are maintained independently of
 * linear allocation mode
 */
struct lsv_segment {
	enum lsv_segment_state state;
	enum lsv_segment_stream stream;
	u64 id; /* segment numberю */
	u64 start_pba; /* first pba in this segment */

	atomic_t next_slot; /* next free pba in this segment */
	atomic_t live; /* block still referenced */
	atomic_t shared; /* block with >= 2 references */
	atomic_t inflight; /* writes begun but not completed. */
	atomic_t hits_in; /* dedup hits into this segment */

	spinlock_t lock; /* protects the segment's state and pblk_count */
};

struct lsv_segment_pool {
	struct lsv_segment *segments;
	unsigned long *free_bitmap;
	u32 nr_segments;
	u32 cells_per_segment;
	u32 free_count;

	struct lsv_segment *open_segments[LSV_SEGMENT_STREAM_NR];

	u32 gc_reserve; /* number of segments reserved for GC */

	spinlock_t pool_lock; /* protects the open_segments and free_bitmap */
};

/* What the device hands lmap to build it from, as configured. */
struct lsv_lmap_params {
	char index_ds[LSV_DS_NAME_LEN + 1];
	u32 cell_size;
	enum lsv_space_layout space_layout;
	u64 segment_size;

	sector_t back_cap_sectors;
};

struct lsv_lmap_pos {
	u64 lba; /* logical cell number */
	u32 offset; /* sector inside that cell */
	u32 sectors; /* sectors left until the end of the cell */
};


struct lsv_lmap {
	struct lsv_ds index;
	char ds_type[LSV_DS_NAME_LEN];

	u32 cell_size; /* bytes */
	u32 cell_sectors;
	u8 cell_sect_shift; /* log2(cell_sectors), a shift over sector numbers */
	u64 capacity_cells;

	u64 segment_size;
	u32 cells_per_segment;
	u32 nr_segments;
	struct lsv_segment_pool segment_pool;

	struct xarray pblk_pba_map; /* Every live pblk by its pba */
	struct ida pba_bitmap;

	struct bio_set bio_set;

	struct lsv_lmap_params params;
};

s32 lsv_lmap_engine_init(void);
void lsv_lmap_engine_deinit(void);

s32 lsv_lmap_init(struct lsv_lmap *lmap);
void lsv_lmap_deinit(struct lsv_lmap *lmap);

struct lsv_pblk *lsv_pblk_alloc(struct lsv_lmap *lmap);
void lsv_pblk_put(struct lsv_lmap *lmap, struct lsv_pblk *pblk);

s32 lsv_lmap_bind(struct lsv_lmap *lmap, u64 lba, struct lsv_pblk *pblk);
bool lsv_lmap_lookup_pba(struct lsv_lmap *lmap, u64 lba, u64 *pba);

s32 lsv_segment_pool_init(struct lsv_lmap *lmap);
void lsv_segment_pool_deinit(struct lsv_lmap *lmap);
s64 lsv_segment_slot(struct lsv_lmap *lmap, enum lsv_segment_stream stream);

/* Where a sector lands inside the cell grid. */
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

static inline enum lsv_space_layout lsv_lmap_is_linear(const struct lsv_lmap *lmap)
{
	return lmap->params.space_layout == LSV_SPACE_LINEAR;
}

static inline enum lsv_space_layout lsv_lmap_is_segmented(const struct lsv_lmap *lmap)
{
	return lmap->params.space_layout == LSV_SPACE_SEGMENTED;
}

#endif
