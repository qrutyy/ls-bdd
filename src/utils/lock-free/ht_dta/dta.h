#ifndef _LSV_DTA_H
#define _LSV_DTA_H

#define LSV_DTA_ANCHOR_THRESHOLD	5	/* drop an anchor every N live cells */

/*
 * Cells worth keeping per participant for node_copy(), out of reach of
 * ordinary inserts: enough for a range of a stalled participant that nothing
 * was inserted into. They are a fast path, not a bound.
 *
 * Recovery copies the live cells of the range it freezes, and freezing cannot
 * be undone, so a recovery that cannot copy cannot finish -- and until it
 * does, the participant it answers keeps the reclamation bounds of the whole
 * table where they are. No number of cells covers every range: besides the
 * anchor and the LSV_DTA_ANCHOR_THRESHOLD cells older than the stalled
 * operation, it holds every cell inserted into it while that operation
 * stalled. Nor do the cells a recovery has spent come back before it is over:
 * the lsvginals it replaced are what the stalled participant may still be
 * standing on. node_copy() must therefore fall back on an allocator the
 * table cannot fill up once the cells kept for it are gone, and may fail only
 * when that allocator does; a recovery it failed is tried again later.
 */
#define LSV_DTA_SPARE_CELLS		(2 * LSV_DTA_ANCHOR_THRESHOLD)

/*
 * Cells one pass of a recovery copies at most. A wider range is replaced a
 * pass at a time, and so is one whose copies node_copy() could not serve in
 * full, so that no range is too wide to recover.
 *
 * It bounds the copies of a pass, not its walks: every pass still walks the
 * whole range left frozen, and the bucket up to it, so replacing a range of N
 * cells walks about N * N / (2 * LSV_DTA_SPARE_LIMIT) of them. A pass that
 * resumes where the last one stopped is a follow-up.
 */
#define LSV_DTA_SPARE_LIMIT		(4 * LSV_DTA_SPARE_CELLS)

#define LSV_DTA_RETIRE_THRESHOLD	3	/* scan the buffer after N retires */
#define LSV_DTA_RECLAIM_BUDGET		32	/* cells examined by one scan */
#define LSV_DTA_RECOVERY_THRESHOLD	256	/* suspect a thread after N blocked scans */
/*
 * ... and only once it has held the same timestamp for this long. [DtA]
 * assumes recovery answers a thread failure, "typically very uncommon in real
 * systems"; a scan count alone also fires for a worker that was merely
 * preempted, and every recovery costs a restarted operation and cells that
 * stay out of reach until the recovery is over.
 */
#define LSV_DTA_RECOVERY_DELAY_MS	1000

/*
 * Low two bits of anchor_and_flags ([DtA] 3, Figure 1):
 *   running:   IDLE=0, STUCK=0
 *   idle:      IDLE=1, STUCK=0
 *   stuck:     IDLE=0, STUCK=1  (suspected by another thread)
 *   recovered: IDLE=1, STUCK=1
 */
#define LSV_DTA_STUCK_BIT	1ULL
#define LSV_DTA_IDLE_BIT	2ULL
#define LSV_DTA_FLAGS_MASK	(LSV_DTA_IDLE_BIT | LSV_DTA_STUCK_BIT)

#define lsv_dta_af_ptr(af)	((struct lsv_lf_list_node *)((af) & ~LSV_DTA_FLAGS_MASK))
#define lsv_dta_af_is_idle(af)	(!!((af) & LSV_DTA_IDLE_BIT))
#define lsv_dta_af_is_stuck(af)	(!!((af) & LSV_DTA_STUCK_BIT))
#define lsv_dta_af_is_suspected(af)	(!lsv_dta_af_is_idle(af) && lsv_dta_af_is_stuck(af))
#define lsv_dta_af_is_recovered(af)	(lsv_dta_af_is_idle(af) && lsv_dta_af_is_stuck(af))
#define lsv_dta_af_make(ptr, idle, stuck)				\
	((u64)(ptr) | ((idle) ? LSV_DTA_IDLE_BIT : 0ULL)		\
		    | ((stuck) ? LSV_DTA_STUCK_BIT : 0ULL))

#endif
