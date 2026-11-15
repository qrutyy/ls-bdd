#ifndef _LSV_HT_IMPL_TEST_H
#define _LSV_HT_IMPL_TEST_H

/* The EBR table (ebr+) for the shared lsv_ht tests: see ht_tests/lsv_kunit_ht.h. */
#include "utils/lock-free/ht_ebr/ht_ebr.h"
#include "utils/lock-free/ht_ebr/ebr.h"
#include "utils/lock-free/ht_ebr/tests/include_static/__static_kunit_lsv_ht.h"

#define LSV_HT_IMPL_NAME		"ebr+"
#define LSV_HT_HAS_RECOVERY		0
#define LSV_HT_OPS_NONPREEMPTIBLE	0
/* the limbo lists are the table's, not a participant's: no buffer is left behind */
#define LSV_HT_IDLE_BUFFERS		0

/* a participant stalled in its region holds back every cell retired from then on */
#define lsv_ht_test_stall_hook		ht_kunit_cell_reached

/* the epoch moves once per LSV_EBR_ADVANCE_FREQ operations, and cells wait two */
#define LSV_HT_TEST_QUIET_RECLAIM_OPS		(4 * LSV_EBR_ADVANCE_FREQ)

#define LSV_HT_TEST_STRESS_RESERVE	0
#define LSV_HT_TEST_TIGHT_RESERVE	0

#endif /* _LSV_HT_IMPL_TEST_H */
