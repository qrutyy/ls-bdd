/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef LSV_MAIN_H
#define LSV_MAIN_H

#include <linux/list.h>
#include <linux/mutex.h>

#include "core/map.h"

struct lsv_mng {
	s32 major;
	struct list_head dev_list;
	struct mutex lock; /* guards dev_list */

	struct lsv_map_cache map_cache; /* shared by every device */
};

extern struct lsv_mng *g_mng;

#endif
