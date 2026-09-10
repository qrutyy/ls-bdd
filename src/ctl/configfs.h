/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef LSV_CTL_CONFIGFS_H
#define LSV_CTL_CONFIGFS_H

#include <linux/configfs.h>
#include <linux/limits.h>
#include <linux/mutex.h>
#include <linux/types.h>

#define LSV_CTL_MAX_BD_NAME_LENGTH 15
#define LSV_CTL_MAX_MINORS_AM 20
#define LSV_CTL_MAX_DS_NAME_LEN 2

struct lsv_dev;

struct lsv_cfg_dev {
	struct config_item item;
	struct mutex lock; /* serialises attributes against create_new */

	char backing_path[PATH_MAX];
	char index_ds[LSV_CTL_MAX_DS_NAME_LEN + 1];
	u32 cell_size;
	u64 segment_size;

	bool created;
	struct lsv_dev *dev; /* set by create_new, torn down on rmdir */
};

s32 lsv_configfs_register(void);
void lsv_configfs_unregister(void);

#endif
