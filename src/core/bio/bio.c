// SPDX-License-Identifier: GPL-2.0-only

#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/err.h>
#include <linux/minmax.h>
#include <linux/slab.h>

#include "core/bio/bio.h"
#include "core/dedup/dedup.h"
#include "core/lmap/lmap.h"
#include "core/trace/lsv_trace.h"

#define LSV_BIO_SUBMIT 0
#define LSV_BIO_DONE 1

s32 lsv_bio_init(struct lsv_dev *dev)
{
	/* The bio sits right behind the pad, so the pad has to keep it aligned. */
	BUILD_BUG_ON(!IS_ALIGNED(sizeof(struct lsv_bio_req), __alignof__(struct bio)));

	return bioset_init(&dev->bio_set, BIO_POOL_SIZE, sizeof(struct lsv_bio_req), BIOSET_NEED_BVECS);
}

void lsv_bio_deinit(struct lsv_dev *dev)
{
	bioset_exit(&dev->bio_set);
}

static inline struct lsv_bio_req *lsv_bio_to_req(struct bio *clone)
{
	return (struct lsv_bio_req *)((char *)clone - sizeof(struct lsv_bio_req));
}

static void lsv_bio_end_io(struct bio *clone)
{
	struct lsv_bio_req *req = lsv_bio_to_req(clone);
	struct bio *orig = req->orig;
	u64 orig_sector = orig->bi_iter.bi_sector;
	u64 size = orig->bi_iter.bi_size;

	orig->bi_status = clone->bi_status;
	bio_endio(orig);
	/* Frees the request with it: |req| must not be touched from here on. */
	bio_put(clone);

	trace_lsv_bio_end_io(orig_sector, size);
}

static struct lsv_bio_req *lsv_bio_req_alloc(struct lsv_dev *dev, struct bio *bio)
{
	struct lsv_bio_req *req;
	struct bio *clone;

	clone = bio_alloc_clone(dev->back.bd, bio, GFP_NOIO, &dev->bio_set);
	if (!clone)
		return NULL;

	/* The pad comes out of the pool as it was left: every field is set here. */
	req = lsv_bio_to_req(clone);
	req->orig = bio;
	req->clone = clone;
	req->dev = dev;
	req->op = bio_op(bio);

	clone->bi_end_io = lsv_bio_end_io;

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
		zero_fill_bio(bio);
		bio_endio(bio);
		return LSV_BIO_DONE;
	}

	bio->bi_iter.bi_sector = lsv_lmap_data_sector(&dev->lmap, pba) + offset;

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
			bio = bio_split(clone, chunk, GFP_NOIO, &req->dev->bio_set);
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

	trace_lsv_submit_bio(bio->bi_iter.bi_sector, bio->bi_iter.bi_size);

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
