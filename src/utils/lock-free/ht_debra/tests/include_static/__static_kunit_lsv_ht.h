/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef _LSV__STATIC_KUNIT_LSV_HT_DEBRA_H
#define _LSV__STATIC_KUNIT_LSV_HT_DEBRA_H

#include <linux/types.h>

#if IS_ENABLED(CONFIG_KUNIT)

struct lsv_lf_list_node;
struct lsv_ht;

void ht_kunit_cell_linking(struct lsv_lf_list_node *cell);

/* not functions: hooks that KUnit sets, see debra.c */
extern void (*g_lsv_ht_kunit_cell_check)(struct lsv_lf_list_node *cell);

bool ht_debra_is_active(const struct lsv_ht *ht, unsigned int tid);
u32 ht_debra_limbo_total(const struct lsv_ht *ht);
bool ht_kunit_reclaim_requests_pending(const struct lsv_ht *ht);
/* the limbo bags of one participant */
u32 ht_debra_limbo_cnt(const struct lsv_ht *ht, unsigned int tid);

#endif /* CONFIG_KUNIT */
#endif /* _LSV__STATIC_KUNIT_LSV_HT_DEBRA_H */
