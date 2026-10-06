/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef LSV_CORE_BIO_H
#define LSV_CORE_BIO_H

#include <linux/bio.h>
#include <linux/blkdev.h>

#include "core/dev.h"

struct lsv_bio_req {
	struct bio *orig;
	struct bio *clone;
	struct lsv_dev *dev;
	enum req_op op;
};

extern const struct block_device_ops lsv_bio_ops;

size_t lsv_bio_copy_buffer(struct bio *bio, void *buf, size_t size, bool to_buffer);

s32 lsv_bio_cache_alloc(void);
void lsv_bio_cache_free(void);


static inline size_t lsv_bio_copy_to_buffer(struct bio *bio, void *buf, size_t size)
{
	return lsv_bio_copy_buffer(bio, buf, size, true);
}

static inline size_t lsv_bio_copy_from_buffer(struct bio *bio, void *buf, size_t size)
{
	return lsv_bio_copy_buffer(bio, buf, size, false);
}

#endif
