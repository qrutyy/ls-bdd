#ifndef _LSV_HT_IMPL_TEST_H
#define _LSV_HT_IMPL_TEST_H

/* The DEBRA table (debra) for the shared lsv_ht tests: see ht_tests/lsv_kunit_ht.h. */
#include "utils/lock-free/ht_debra/ht_debra.h"
#include "utils/lock-free/ht_debra/debra.h"
#include "utils/lock-free/ht_debra/tests/include_static/__static_kunit_lsv_ht.h"

#define LSV_HT_IMPL_NAME		"debra"
#define LSV_HT_HAS_RECOVERY		0
#define LSV_HT_OPS_NONPREEMPTIBLE	0

/*
 * Reclamation runs only in the retirer's own operations, when the epoch has
 * moved: never at the end of an operation that ran out of cells, and never on
 * behalf of a participant that went idle.
 */
#define LSV_HT_RECLAIM_ON_DEMAND	0

/*
 * A cell waits three rotations, each costing max(nr_threads, INCR) operations,
 * so a stress run holds an order of magnitude more cells than an ebr one.
 */
#define LSV_HT_TEST_POOL_PER_JOB	2048

/* a participant stalled in its region holds back every cell retired from then on */
#define lsv_ht_test_stall_hook		ht_kunit_cell_reached

/*
 * The epoch moves once every max(nr_threads, LSV_INCR_THRESHOLD) checks, and a
 * cell waits three rotations, so a quiet table needs that many operations four
 * times over. nr_threads is the participant limit of the table, not a constant.
 */
struct lsv_ht_test_ctx;
long lsv_ht_test_quiet_ops(const struct lsv_ht_test_ctx *ctx);

#define LSV_HT_TEST_QUIET_RECLAIM_OPS	lsv_ht_test_quiet_ops(ctx)

#define LSV_HT_TEST_STRESS_RESERVE	0
#define LSV_HT_TEST_TIGHT_RESERVE	0

#endif /* _LSV_HT_IMPL_TEST_H */
