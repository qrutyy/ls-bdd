#ifndef _LSV_HT_IMPL_TEST_H
#define _LSV_HT_IMPL_TEST_H

/* The DtA table for the shared lsv_ht tests: see ht_tests/lsv_kunit_ht.h. */
#include "utils/lock-free/ht_dta/ht_dta.h"
#include "utils/lock-free/ht_dta/dta.h"
#include "utils/lock-free/ht_dta/tests/include_static/__static_kunit_lsv_ht.h"

#define LSV_HT_IMPL_NAME		"dta"
#define LSV_HT_HAS_RECOVERY		1
#define LSV_HT_OPS_NONPREEMPTIBLE	0

/* a participant stalled past its anchor holds back reclamation until recovered */
#define lsv_ht_test_stall_hook		ht_kunit_anchor_dropped

/* Cells node_copy() may take on top of the pool, as a real consumer keeps. */
#define LSV_HT_TEST_RECOVERY_RESERVE	(4 * LSV_DTA_SPARE_LIMIT)
#define LSV_HT_TEST_STRESS_RESERVE	LSV_HT_TEST_RECOVERY_RESERVE
#define LSV_HT_TEST_TIGHT_RESERVE	LSV_DTA_SPARE_CELLS

#endif /* _LSV_HT_IMPL_TEST_H */
