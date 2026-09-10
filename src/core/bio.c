// SPDX-License-Identifier: GPL-2.0-only

#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/minmax.h>
#include <linux/slab.h>

#include "core/bio.h"
#include "core/map.h"

/* Outcome of remapping a single cell sized bio. */
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
	struct bio *orig = req->orig;

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

	req->clone = bio_alloc_clone(dev->back.bd, bio, GFP_NOIO, &dev->map.bio_set);
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
 * Every write lands on a freshly allocated physical cell, so the mapping is
 * repointed before the bio is handed to the backing device.
 */
static s32 lsv_bio_setup_write(struct lsv_dev *dev, struct bio *bio, u64 lba, u32 offset, u32 sectors)
{
	u64 pba;
	s32 rc;

	if (offset || sectors != dev->map.cell_sectors) {
		/* TODO: read-modify-write for partial cell writes. */
		pr_warn_once("lsv: partial cell write is not implemented yet\n");
		return -EOPNOTSUPP;
	}

	rc = lsv_map_remap(&dev->map, lba, &pba);
	if (rc)
		return rc;

	bio->bi_iter.bi_sector = lsv_map_data_sector(&dev->map, pba);

	return LSV_BIO_SUBMIT;
}

/*
 * A cell that was never written has no mapping at all, so the read is served
 * with zeroes instead of being forwarded.
 */
static s32 lsv_bio_setup_read(struct lsv_dev *dev, struct bio *bio, u64 lba, u32 offset)
{
	struct lsv_cell *cell;

	cell = lsv_map_lookup(&dev->map, lba);
	if (!cell) {
		zero_fill_bio(bio);
		bio_endio(bio);
		return LSV_BIO_DONE;
	}

	bio->bi_iter.bi_sector = lsv_map_data_sector(&dev->map, READ_ONCE(cell->pba)) + offset;

	return LSV_BIO_SUBMIT;
}

static s32 lsv_bio_setup_bio(struct lsv_bio_req *req, struct bio *bio, u64 lba, u32 offset, u32 sectors)
{
	if (req->op == REQ_OP_READ)
		return lsv_bio_setup_read(req->dev, bio, lba, offset);

	if (req->op == REQ_OP_WRITE)
		return lsv_bio_setup_write(req->dev, bio, lba, offset, sectors);

	return -EOPNOTSUPP;
}

/*
 * Walks the request cell by cell: everything that crosses a cell boundary is
 * split off, remapped on its own and chained back to the parent clone.
 */
static void lsv_bio_process(struct lsv_bio_req *req)
{
	struct lsv_map *map = &req->dev->map;
	struct bio *clone = req->clone;
	struct bio *bio;
	s32 rc;

	/* Flushes and other payload free requests are passed straight through. */
	if (!bio_sectors(clone)) {
		submit_bio_noacct(clone);
		return;
	}

	while (true) {
		struct lsv_map_pos pos;
		u32 chunk;

		lsv_map_locate(map, clone->bi_iter.bi_sector, &pos);
		chunk = min_t(u32, pos.sectors, bio_sectors(clone));

		bio = clone;

		if (chunk < bio_sectors(clone)) {
			bio = bio_split(clone, chunk, GFP_NOIO, &map->bio_set);
			if (!bio) {
				rc = -ENOMEM;
				goto err;
			}
			bio_chain(bio, clone);
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

static void lsv_submit_bio(struct bio *bio)
{
	struct lsv_dev *dev = bio->bi_bdev->bd_disk->private_data;
	struct lsv_bio_req *req;

	if (unlikely(!dev)) {
		bio_io_error(bio);
		return;
	}

	req = lsv_bio_req_alloc(dev, bio);
	if (unlikely(!req)) {
		bio_io_error(bio);
		return;
	}

	lsv_bio_process(req);
}

const struct block_device_operations lsv_bio_ops = {
	.owner = THIS_MODULE,
	.submit_bio = lsv_submit_bio,
};
