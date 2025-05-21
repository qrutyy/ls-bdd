/* SPDX-License-Identifier: GPL-2.0-only */

#pragma once

#define LSBDD_MAX_BD_NAME_LENGTH 15
#define LSBDD_MAX_MINORS_AM 20
#define LSBDD_MAX_DS_NAME_LEN 2
#define LSBDD_BLKDEV_NAME_PREFIX "lsvbd"
#define LSBDD_SECTOR_OFFSET 32

static const char *available_ds[] = { "bt", "sl", "ht", "rb" };

// Returns "ret_val" if el == NULL
#define IF_NULL_RETURN(el, ret_val)                                                                                                        \
	do {                                                                                                                               \
		if (!el)                                                                                                                   \
			return ret_val;                                                                                                    \
	} while (0)

struct lsbdd_value_redir {
	sector_t redirected_sector;
	u32 block_size;
};

// Block device manager structure for saving the linked meta data
struct lsbdd_bd_manager {
	char *vbd_name;
	struct gendisk *vbd_disk;
	struct bdev_handle *bd_handler;
	struct data_struct *sel_data_struct;
	struct list_head list;
};
