#undef TRACE_SYSTEM
#define TRACE_SYSTEM lsv

#if !defined(_LSV_TRACE_H) || defined(TRACE_HEADER_MULTI_READ)
#define _LSV_TRACE_H

#include <linux/tracepoint.h>

TRACE_EVENT(lsv_submit_bio,
        TP_PROTO(u64 orig_sector, u64 size),
        TP_ARGS(orig_sector, size),
        TP_STRUCT__entry(
                __field(u64, orig_sector)
                __field(u64, size)
        ),
        TP_fast_assign(
                __entry->orig_sector = orig_sector;
                __entry->size = size;
        ),
        TP_printk("orig_sector=%llu size=%llu",
                  __entry->orig_sector, __entry->size)
);

TRACE_EVENT(lsv_bio_end_io,
        TP_PROTO(u64 orig_sector, u64 size),
        TP_ARGS(orig_sector, size),
        TP_STRUCT__entry(
                __field(u64, orig_sector)
                __field(u64, size)
        ),
        TP_fast_assign(
                __entry->orig_sector = orig_sector;
                __entry->size = size;
        ),
        TP_printk("orig_sector=%llu size=%llu",
                  __entry->orig_sector, __entry->size)
);

#endif /* _TRACE_SUBSYS_H */

#undef TRACE_INCLUDE_PATH
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_PATH .
#define TRACE_INCLUDE_FILE lsv_trace

#include <trace/define_trace.h>
