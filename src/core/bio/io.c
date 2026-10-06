// SPDX-License-Identifier: GPL-2.0-only

#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/highmem.h>
#include <linux/minmax.h>
#include <linux/sched.h>

#include "core/bio/io.h"

/*
 * |buf| must be directly mapped memory (kmalloc or a slab cache, not vmalloc)
 * and aligned to the backing device's DMA alignment.
 */
s32 lsv_io_sync(struct block_device *bd, enum req_op op, sector_t sector, void *buf, size_t len)
{
	might_sleep();

	if (WARN_ON_ONCE(current->bio_list))
		return -EDEADLK;

	return bdev_rw_virt(bd, sector, buf, len, op);
}

size_t lsv_bio_copy_buffer(struct bio *bio, void *buf, size_t size, bool to_buffer)
{
	struct bio_vec bv;
	struct bvec_iter bvec_iter;
	size_t len, copied;

	copied = 0;
	bio_for_each_segment(bv, bio, bvec_iter) {
		if (!size)
			break;

		len = min_t(size_t, bv.bv_len, size);

		pr_info("bio=%p copy %s buf=%p: bv_page=%p bv_offset=%u bv_len=%u len=%lu copied=%lu size=%lu\n",
			bio, to_buffer ? "to" : "from", buf,
			bv.bv_page, bv.bv_offset, bv.bv_len, len, copied, size);

		if (to_buffer)
			memcpy_from_page((char *)buf + copied, bv.bv_page, bv.bv_offset, len);
		else
			memcpy_to_page(bv.bv_page, bv.bv_offset, (char *)buf + copied, len);

		size -= len;
		copied += len;
	}

	return copied;
}
