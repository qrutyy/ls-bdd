/* SPDX-License-Identifier: GPL-2.0-only */

#undef TRACE_SYSTEM
#define TRACE_SYSTEM lsv

#if !defined(LSV_CORE_STATS_TRACE_H) || defined(TRACE_HEADER_MULTI_READ)
#define LSV_CORE_STATS_TRACE_H

#include <linux/blk_types.h>
#include <linux/tracepoint.h>
#include <linux/types.h>

/*
 * Per-request events, for what the aggregated stats cannot answer: which
 * requests were slow and what happened to them on the way. They cost nothing
 * until enabled, e.g.
 *
 *	echo 1 > /sys/kernel/tracing/events/lsv/enable
 *	bpftrace -e 'tracepoint:lsv:lsv_bio_complete { @[args->op] = hist(args->lat_ns); }'
 */

TRACE_EVENT(lsv_bio_submit,
	TP_PROTO(dev_t dev, u32 op, sector_t sector, u32 sectors),
	TP_ARGS(dev, op, sector, sectors),

	TP_STRUCT__entry(
		__field(dev_t, dev)
		__field(u32, op)
		__field(sector_t, sector)
		__field(u32, sectors)
	),

	TP_fast_assign(
		__entry->dev = dev;
		__entry->op = op;
		__entry->sector = sector;
		__entry->sectors = sectors;
	),

	TP_printk("%d,%d op=%u %llu + %u", MAJOR(__entry->dev), MINOR(__entry->dev), __entry->op,
		  (unsigned long long)__entry->sector, __entry->sectors)
);

/* One cell-sized chunk of a request: where it went, if anywhere. */
TRACE_EVENT(lsv_bio_remap,
	TP_PROTO(dev_t dev, u32 op, u64 lba, u32 offset, u32 sectors, u64 pba, bool mapped),
	TP_ARGS(dev, op, lba, offset, sectors, pba, mapped),

	TP_STRUCT__entry(
		__field(dev_t, dev)
		__field(u32, op)
		__field(u64, lba)
		__field(u32, offset)
		__field(u32, sectors)
		__field(u64, pba)
		__field(bool, mapped)
	),

	TP_fast_assign(
		__entry->dev = dev;
		__entry->op = op;
		__entry->lba = lba;
		__entry->offset = offset;
		__entry->sectors = sectors;
		__entry->pba = pba;
		__entry->mapped = mapped;
	),

	TP_printk("%d,%d op=%u lba=%llu+%u/%u pba=%llu mapped=%d", MAJOR(__entry->dev), MINOR(__entry->dev),
		  __entry->op, __entry->lba, __entry->offset, __entry->sectors, __entry->pba, __entry->mapped)
);

/* |lat_ns| is 0 when the request was submitted with stats and this event off. */
TRACE_EVENT(lsv_bio_complete,
	TP_PROTO(dev_t dev, u32 op, sector_t sector, u32 sectors, s32 error, u64 lat_ns),
	TP_ARGS(dev, op, sector, sectors, error, lat_ns),

	TP_STRUCT__entry(
		__field(dev_t, dev)
		__field(u32, op)
		__field(sector_t, sector)
		__field(u32, sectors)
		__field(s32, error)
		__field(u64, lat_ns)
	),

	TP_fast_assign(
		__entry->dev = dev;
		__entry->op = op;
		__entry->sector = sector;
		__entry->sectors = sectors;
		__entry->error = error;
		__entry->lat_ns = lat_ns;
	),

	TP_printk("%d,%d op=%u %llu + %u error=%d lat=%lluns", MAJOR(__entry->dev), MINOR(__entry->dev),
		  __entry->op, (unsigned long long)__entry->sector, __entry->sectors, __entry->error,
		  __entry->lat_ns)
);

#define LSV_TRACE_DEDUP_NEW 0
#define LSV_TRACE_DEDUP_CANDIDATE 1
#define LSV_TRACE_DEDUP_HIT 2
#define LSV_TRACE_DEDUP_COLLISION 3

/* Keyed by the backing device: dedup does not know the front one. */
TRACE_EVENT(lsv_dedup,
	TP_PROTO(dev_t back, u64 lba, u64 digest, u32 outcome),
	TP_ARGS(back, lba, digest, outcome),

	TP_STRUCT__entry(
		__field(dev_t, back)
		__field(u64, lba)
		__field(u64, digest)
		__field(u32, outcome)
	),

	TP_fast_assign(
		__entry->back = back;
		__entry->lba = lba;
		__entry->digest = digest;
		__entry->outcome = outcome;
	),

	TP_printk("back=%d,%d lba=%llu digest=%016llx %s", MAJOR(__entry->back), MINOR(__entry->back),
		  __entry->lba, __entry->digest,
		  __print_symbolic(__entry->outcome,
				   { LSV_TRACE_DEDUP_NEW, "new" },
				   { LSV_TRACE_DEDUP_CANDIDATE, "candidate" },
				   { LSV_TRACE_DEDUP_HIT, "hit" },
				   { LSV_TRACE_DEDUP_COLLISION, "collision" }))
);

#endif /* LSV_CORE_STATS_TRACE_H */

#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE lsv_trace
#include <trace/define_trace.h>
