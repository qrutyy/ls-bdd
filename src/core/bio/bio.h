/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef LSV_CORE_BIO_H
#define LSV_CORE_BIO_H

#include <linux/bio.h>
#include <linux/blkdev.h>

#include "core/dev.h"

/*
 * Lives in the front pad of the clone, in the same mempool backed allocation:
 * a clone always comes with its request, and both go away with bio_put().
 */
struct lsv_bio_req {
	struct bio *orig;
	struct bio *clone;
	struct lsv_dev *dev;
	enum req_op op;
};

extern const struct block_device_operations lsv_bio_ops;

s32 lsv_bio_init(struct lsv_dev *dev);
void lsv_bio_deinit(struct lsv_dev *dev);

#endif
