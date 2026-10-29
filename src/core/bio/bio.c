// SPDX-License-Identifier: GPL-2.0-only

#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/err.h>
#include <linux/minmax.h>
#include <linux/slab.h>

#include "core/bio/bio.h"
#include "core/dedup/dedup.h"
#include "core/lmap/lmap.h"
#include "core/stats/lsv_trace.h"
#include "core/stats/stats.h"

#define LSV_BIO_SUBMIT 0
#define LSV_BIO_DONE 1

static struct kmem_cache *g_lsv_bio_req_cachep;

s32 lsv_bio_cache_alloc(void)
{
	g_lsv_bio_req_cachep = kmem_cache_create("lsv_bio_req", sizeof(struct lsv_bio_req), 0, SLAB_HWCACHE_ALIGN, NULL);
	if (!g_lsv_bio_req_cachep)
		return -ENOMEM;

	return 0;
}

void lsv_bio_cache_free(void)
{
	kmem_cache_destroy(g_lsv_bio_req_cachep);
	g_lsv_bio_req_cachep = NULL;
}

static void lsv_bio_end_io(struct bio *clone)
{
	struct lsv_bio_req *req = clone->bi_private;
	struct lsv_dev *dev = req->dev;
	struct bio *orig = req->orig;
	u64 lat = 0;

	if (req->start) {
		lat = local_clock() - req->start;
		if (lsv_stats_enabled())
			lsv_stats_record(&dev->stats, LSV_LAT_BIO, lat);
	}

	if (clone->bi_status)
		lsv_stats_inc(&dev->stats, LSV_CNT_BIO_ERROR);

	trace_lsv_bio_complete(disk_devt(dev->front.disk), req->op, orig->bi_iter.bi_sector,
			       bio_sectors(orig), blk_status_to_errno(clone->bi_status), lat);

	orig->bi_status = clone->bi_status;
	bio_endio(orig);
	bio_put(clone);
	kmem_cache_free(g_lsv_bio_req_cachep, req);
}

static struct lsv_bio_req *lsv_bio_req_alloc(struct lsv_dev *dev, struct bio *bio)
{
	struct lsv_bio_req *req;

	req = kmem_cache_zalloc(g_lsv_bio_req_cachep, GFP_NOIO);
	if (!req)
		return NULL;

	req->clone = bio_alloc_clone(dev->back.bd, bio, GFP_NOIO, &dev->lmap.bio_set);
	if (!req->clone) {
		kmem_cache_free(g_lsv_bio_req_cachep, req);
		return NULL;
	}

	req->orig = bio;
	req->dev = dev;
	req->op = bio_op(bio);

	req->clone->bi_private = req;
	req->clone->bi_end_io = lsv_bio_end_io;

	return req;
}

/*
 * Lets dedup pick the physical cell. Returns LSV_BIO_SUBMIT with *|pblk| set when
 * the data is to be written out, LSV_BIO_DONE when dedup took the bio over, or a
 * negative error.
 */
static s32 lsv_bio_setup_dedup_write(struct lsv_dev *dev, struct bio *bio, u64 lba, struct lsv_pblk **pblk)
{
	s32 rc;

	rc = lsv_dedup_write(dev->dedup, bio, lba, pblk);
	if (rc < 0)
		return rc;

	if (!lsv_de_is_new(rc))
		return LSV_BIO_DONE;

	return LSV_BIO_SUBMIT;
}

static s32 lsv_bio_setup_write(struct lsv_dev *dev, struct bio *bio, u64 lba, u32 offset, u32 sectors)
{
	struct lsv_lmap *lmap = &dev->lmap;
	struct lsv_pblk *pblk;
	u64 pba;
	s32 rc;

	if (offset || sectors != lmap->cell_sectors) {
		/* TODO: read-modify-write for partial cell writes. */
		lsv_stats_inc(&dev->stats, LSV_CNT_PARTIAL_WRITE);
		pr_warn_once("lsv: partial cell write is not implemented yet\n");
		return -EOPNOTSUPP;
	}

	if (dev->dedup) {
		/* Either an error, or the bio is not ours to submit any more. */
		rc = lsv_bio_setup_dedup_write(dev, bio, lba, &pblk);
		if (rc)
			return rc;
	} else {
		pblk = lsv_pblk_alloc(lmap);
		if (IS_ERR(pblk))
			return PTR_ERR(pblk);
	}

	/*
	 * read before binding: from then on the reference is the cell's, and a
	 * concurrent overwrite of this lba may drop it and free the pblk.
	 */
	pba = pblk->pba;

	rc = lsv_lmap_bind(lmap, lba, pblk);
	if (rc) {
		lsv_pblk_put(lmap, pblk);
		return rc;
	}

	bio->bi_iter.bi_sector = lsv_lmap_data_sector(lmap, pba);
	trace_lsv_bio_remap(disk_devt(dev->front.disk), REQ_OP_WRITE, lba, offset, sectors, pba, true);

	return LSV_BIO_SUBMIT;
}

/*
 * A cell that was never written has no mapping at all, so the read is served
 * with zeroes instead of being forwarded.
 */
static s32 lsv_bio_setup_read(struct lsv_dev *dev, struct bio *bio, u64 lba, u32 offset)
{
	u64 pba;

	if (!lsv_lmap_lookup_pba(&dev->lmap, lba, &pba)) {
		lsv_stats_inc(&dev->stats, LSV_CNT_READ_UNMAPPED);
		trace_lsv_bio_remap(disk_devt(dev->front.disk), REQ_OP_READ, lba, offset, bio_sectors(bio), 0,
				    false);
		zero_fill_bio(bio);
		bio_endio(bio);
		return LSV_BIO_DONE;
	}

	bio->bi_iter.bi_sector = lsv_lmap_data_sector(&dev->lmap, pba) + offset;
	trace_lsv_bio_remap(disk_devt(dev->front.disk), REQ_OP_READ, lba, offset, bio_sectors(bio), pba, true);

	return LSV_BIO_SUBMIT;
}

static s32 lsv_bio_setup_bio(struct lsv_bio_req *req, struct bio *bio, u64 lba, u32 offset, u32 sectors)
{
	struct lsv_stats *stats = &req->dev->stats;
	u64 start = lsv_stats_now();
	s32 rc;

	if (req->op == REQ_OP_READ) {
		rc = lsv_bio_setup_read(req->dev, bio, lba, offset);
		lsv_stats_since(stats, LSV_LAT_REMAP_READ, start);
		return rc;
	}

	if (req->op == REQ_OP_WRITE) {
		rc = lsv_bio_setup_write(req->dev, bio, lba, offset, sectors);
		lsv_stats_since(stats, LSV_LAT_REMAP_WRITE, start);
		return rc;
	}

	return -EOPNOTSUPP;
}

/*
 * Walks the request cell by cell: everything that crosses a cell boundary is
 * split off, remapped on its own and chained back to the parent clone.
 */
static void lsv_bio_process(struct lsv_bio_req *req)
{
	struct lsv_lmap *lmap = &req->dev->lmap;
	struct bio *clone = req->clone;
	struct bio *bio;
	s32 rc;

	/* Flushes and other payload free requests are passed straight through. */
	if (!bio_sectors(clone)) {
		submit_bio_noacct(clone);
		return;
	}

	while (true) {
		struct lsv_lmap_pos pos;
		u32 chunk;

		lsv_lmap_locate(lmap, clone->bi_iter.bi_sector, &pos);
		chunk = min_t(u32, pos.sectors, bio_sectors(clone));

		bio = clone;

		if (chunk < bio_sectors(clone)) {
			bio = bio_split(clone, chunk, GFP_NOIO, &lmap->bio_set);
			if (!bio) {
				rc = -ENOMEM;
				goto err;
			}
			bio_chain(bio, clone);
			lsv_stats_inc(&req->dev->stats, LSV_CNT_SPLIT);
		}

		rc = lsv_bio_setup_bio(req, bio, pos.lba, pos.offset, chunk);
		if (rc < 0)
			goto err;

		if (rc == LSV_BIO_SUBMIT)
			submit_bio_noacct(bio);

		if (bio == clone)
			return;
	}

err:
	if (bio && bio != clone)
		bio_io_error(bio);

	bio_io_error(clone);
}

static void lsv_bio_count(struct lsv_dev *dev, struct bio *bio)
{
	switch (bio_op(bio)) {
	case REQ_OP_READ:
		lsv_stats_inc(&dev->stats, LSV_CNT_BIO_READ);
		break;
	case REQ_OP_WRITE:
		lsv_stats_inc(&dev->stats, LSV_CNT_BIO_WRITE);
		break;
	default:
		lsv_stats_inc(&dev->stats, LSV_CNT_BIO_OTHER);
	}
}

static void lsv_submit_bio(struct bio *bio)
{
	struct lsv_dev *dev = bio->bi_bdev->bd_disk->private_data;
	struct lsv_bio_req *req;
	u64 start;

	if (unlikely(!dev)) {
		bio_io_error(bio);
		return;
	}

	/* The completion event reports latency too, so it clocks the request when on. */
	start = trace_lsv_bio_complete_enabled() ? local_clock() : lsv_stats_now();

	lsv_bio_count(dev, bio);
	trace_lsv_bio_submit(disk_devt(dev->front.disk), bio_op(bio), bio->bi_iter.bi_sector, bio_sectors(bio));

	req = lsv_bio_req_alloc(dev, bio);
	if (unlikely(!req)) {
		lsv_stats_inc(&dev->stats, LSV_CNT_BIO_ERROR);
		bio_io_error(bio);
		return;
	}
	req->start = start;

	/* |req| may be completed and freed by the time this returns, |dev| may not. */
	lsv_bio_process(req);

	lsv_stats_since(&dev->stats, LSV_LAT_SUBMIT, start);
}

const struct block_device_operations lsv_bio_ops = {
	.owner = THIS_MODULE,
	.submit_bio = lsv_submit_bio,
};
