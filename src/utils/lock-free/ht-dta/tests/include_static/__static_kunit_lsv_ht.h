#ifndef _LSV__STATIC_KUNIT_LSV_HT_H
#define _LSV__STATIC_KUNIT_LSV_HT_H

#include <linux/types.h>

#if IS_ENABLED(CONFIG_KUNIT)

/* These functions are static outside KUnit builds; ht_cell_reached() never is. */
struct lsv_lf_list_node;

void ht_anchor_dropped(struct lsv_lf_list_node *anchor);
void ht_cell_reached(struct lsv_lf_list_node *cell);
void ht_cell_linking(struct lsv_lf_list_node *cell);
void ht_record_flags_read(unsigned int tid);
void ht_begin_computed(unsigned int tid);

/* not a function: a hook that KUnit sets, see dta.c */
extern void (*g_lsv_ht_cell_check)(struct lsv_lf_list_node *cell);

struct lsv_ht;

bool ht_participant_holds_anchor(const struct lsv_ht *ht, unsigned int tid);
bool ht_participant_is_suspected(const struct lsv_ht *ht, unsigned int tid);
u64 ht_recovery_aborts(const struct lsv_ht *ht);
u64 ht_recovery_walked(const struct lsv_ht *ht);
bool ht_reclaim_requests_pending(const struct lsv_ht *ht);

#endif /* CONFIG_KUNIT */
#endif /* _LSV__STATIC_KUNIT_LSV_HT_H */
