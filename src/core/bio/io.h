/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef LSV_CORE_BIO_IO_H
#define LSV_CORE_BIO_IO_H

#include <linux/bio.h>
#include <linux/blkdev.h>

/*
 * Requests the driver issues to the backing device on its own behalf, as
 * opposed to the user bios it only redirects.
 */

s32 lsv_io_sync(struct block_device *bd, enum req_op op, sector_t sector, void *buf, size_t len);

/*
 * Synchronous, so they sleep until the backing device answers. Never call them
 * from ->submit_bio: a bio submitted there is only queued on current->bio_list
 * and is not dispatched until ->submit_bio returns, so waiting for it deadlocks.
 * Run them from a worker instead.
 */
static inline s32 lsv_io_read(struct block_device *bd, sector_t sector, void *buf, size_t len)
{
	return lsv_io_sync(bd, REQ_OP_READ, sector, buf, len);
}

static inline s32 lsv_io_write(struct block_device *bd, sector_t sector, void *buf, size_t len)
{
	return lsv_io_sync(bd, REQ_OP_WRITE, sector, buf, len);
}

/* Moving a bio's payload in and out of a linear buffer. */

size_t lsv_bio_copy_buffer(struct bio *bio, void *buf, size_t size, bool to_buffer);

static inline size_t lsv_bio_copy_to_buffer(struct bio *bio, void *buf, size_t size)
{
	return lsv_bio_copy_buffer(bio, buf, size, true);
}

static inline size_t lsv_bio_copy_from_buffer(struct bio *bio, void *buf, size_t size)
{
	return lsv_bio_copy_buffer(bio, buf, size, false);
}

#endif
