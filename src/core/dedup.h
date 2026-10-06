/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef LSV_CORE_DEDUP_H
#define LSV_CORE_DEDUP_H

#include "utils/ds_control.h"

struct lsv_dedup_map {
	struct lsv_ds map;
};

struct lsv_dedup {
	struct lsv_dedup_map *map;
};

#endif
