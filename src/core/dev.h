/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef LSV_CORE_DEV_H
#define LSV_CORE_DEV_H

#include <linux/blkdev.h>
#include <linux/list.h>

#include "core/map.h"

#define LSV_BLKDEV_NAME_PREFIX "lsv"
#define LSV_MAX_MINORS 64

struct lsv_front {
	char name[DISK_NAME_LEN];
	struct gendisk *disk;
	s32 minor;
};

struct lsv_back {
	char *path;
	struct file *bd_file;
	struct block_device *bd;
};

/*
 * Everything the core needs in order to instantiate a device. Filled in by the
 * control layer; deliberately free of any configfs types.
 */
struct lsv_dev_params {
	const char *name; /* device identity, becomes disk_name */
	const char *back_path;
	const char *index_ds;
	u32 cell_size;
	u64 segment_size;
};

struct lsv_dev {
	struct lsv_front front;
	struct lsv_back back;
	struct lsv_map map;

	struct list_head node; /* node in the module wide device list */
};

s32 lsv_dev_create(const struct lsv_dev_params *params, struct lsv_dev **out);
void lsv_dev_destroy(struct lsv_dev *dev);

#endif
