/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef LSV_CORE_STATS_H
#define LSV_CORE_STATS_H

#include <linux/jump_label.h>
#include <linux/percpu.h>
#include <linux/sched/clock.h>
#include <linux/types.h>

/*
 * Per-device counters and latency histograms of the stages a request goes
 * through, the part of the latency fio cannot see. Kept per CPU, so the hot
 * path only ever touches its own cache lines; summed up when read.
 *
 * Two switches, so that the cost of the instrumentation itself can be
 * measured: building with stats=0 compiles it out, and loading with
 * stats_enabled=0 turns every probe into a single patched-out branch.
 */

enum lsv_stat_cnt {
	LSV_CNT_BIO_READ, /* front bios, by op */
	LSV_CNT_BIO_WRITE,
	LSV_CNT_BIO_OTHER, /* flushes and other payload free requests, passed through */
	LSV_CNT_BIO_ERROR,
	LSV_CNT_SPLIT, /* chunks split off a front bio at cell boundaries */
	LSV_CNT_READ_UNMAPPED, /* read chunks served with zeroes */
	LSV_CNT_CELL_NEW, /* index insertions */
	LSV_CNT_CELL_OVERWRITE, /* writes to an lba that was already mapped */
	LSV_CNT_PBLK_ALLOC,
	LSV_CNT_PBLK_FREE,
	LSV_CNT_ENOSPC, /* no physical cell left */
	LSV_CNT_PARTIAL_WRITE, /* rejected, read-modify-write is not there yet */
	LSV_CNT_DEDUP_NEW, /* digest seen for the first time */
	LSV_CNT_DEDUP_CANDIDATE, /* digest matched, the write went to verify */
	LSV_CNT_DEDUP_HIT, /* verified duplicate, no data written */
	LSV_CNT_DEDUP_COLLISION, /* digest matched, bytes did not */
	LSV_CNT_DEDUP_SKIPPED, /* no buffer or no usable digest: stored plainly */
	LSV_CNT_NR,
};

enum lsv_stat_lat {
	LSV_LAT_BIO, /* front bio, from ->submit_bio to its completion */
	LSV_LAT_SUBMIT, /* CPU time of ->submit_bio itself */
	LSV_LAT_REMAP_READ, /* one read chunk: lookup and redirect */
	LSV_LAT_REMAP_WRITE, /* one write chunk: allocation, dedup, bind */
	LSV_LAT_IDX_LOOKUP, /* lsv_ds_lookup() */
	LSV_LAT_IDX_INSERT, /* lsv_ds_insert() */
	LSV_LAT_DEDUP_DIGEST, /* hashing the payload */
	LSV_LAT_DEDUP_VERIFY, /* the verify worker, read back and compare */
	LSV_LAT_NR,
};

/*
 * Log-linear buckets: values below 2^SUB_BITS get one bucket each, above that
 * every power of two is cut into 2^SUB_BITS equal parts, so a bucket is at
 * most 1/16 (~6%) of its value wide. Values from 2^LSV_STATS_HIST_MAX_SHIFT ns
 * (~18 minutes) on share the last bucket.
 */
#define LSV_STATS_HIST_SUB_BITS 4
#define LSV_STATS_HIST_SUB (1U << LSV_STATS_HIST_SUB_BITS)
#define LSV_STATS_HIST_MAX_SHIFT 40
#define LSV_STATS_HIST_BUCKETS ((LSV_STATS_HIST_MAX_SHIFT - LSV_STATS_HIST_SUB_BITS + 1) << LSV_STATS_HIST_SUB_BITS)

struct lsv_stats_hist {
	u64 count;
	u64 sum; /* ns */
	u64 buckets[LSV_STATS_HIST_BUCKETS];
};

struct lsv_stats_cpu {
	u64 cnt[LSV_CNT_NR];
	struct lsv_stats_hist lat[LSV_LAT_NR];
};

struct lsv_stats {
	struct lsv_stats_cpu __percpu *pcpu;
};

struct lsv_dev;
struct dentry;

static inline u32 lsv_stats_bucket(u64 v)
{
	u32 shift;

	if (v < LSV_STATS_HIST_SUB)
		return v;

	if (v >= (1ULL << LSV_STATS_HIST_MAX_SHIFT))
		return LSV_STATS_HIST_BUCKETS - 1;

	shift = fls64(v) - 1 - LSV_STATS_HIST_SUB_BITS;

	return ((shift + 1) << LSV_STATS_HIST_SUB_BITS) + ((v >> shift) & (LSV_STATS_HIST_SUB - 1));
}

/* The smallest value that falls into |bucket|. */
static inline u64 lsv_stats_bucket_floor(u32 bucket)
{
	u32 shift;

	if (bucket < LSV_STATS_HIST_SUB)
		return bucket;

	shift = (bucket >> LSV_STATS_HIST_SUB_BITS) - 1;

	return (u64)(LSV_STATS_HIST_SUB + (bucket & (LSV_STATS_HIST_SUB - 1))) << shift;
}

#ifdef LSV_STATS

DECLARE_STATIC_KEY_TRUE(lsv_stats_on);

static inline bool lsv_stats_enabled(void)
{
	return static_branch_likely(&lsv_stats_on);
}

static inline void lsv_stats_inc(struct lsv_stats *stats, enum lsv_stat_cnt cnt)
{
	if (lsv_stats_enabled())
		this_cpu_inc(stats->pcpu->cnt[cnt]);
}

/* A timestamp for lsv_stats_since(); 0 when stats are off, so nothing is clocked. */
static inline u64 lsv_stats_now(void)
{
	return lsv_stats_enabled() ? local_clock() : 0;
}

static inline void lsv_stats_record(struct lsv_stats *stats, enum lsv_stat_lat lat, u64 ns)
{
	struct lsv_stats_hist __percpu *hist = &stats->pcpu->lat[lat];

	this_cpu_inc(hist->count);
	this_cpu_add(hist->sum, ns);
	this_cpu_inc(hist->buckets[lsv_stats_bucket(ns)]);
}

static inline void lsv_stats_since(struct lsv_stats *stats, enum lsv_stat_lat lat, u64 start)
{
	u64 now;

	if (!lsv_stats_enabled() || !start)
		return;

	now = local_clock();
	/* local_clock() may go back a little when the task migrated */
	lsv_stats_record(stats, lat, now > start ? now - start : 0);
}

s32 lsv_stats_init(struct lsv_stats *stats);
void lsv_stats_free(struct lsv_stats *stats);

s32 lsv_stats_debugfs_init(void);
void lsv_stats_debugfs_deinit(void);
struct dentry *lsv_stats_debugfs_add(struct lsv_dev *dev);
void lsv_stats_debugfs_remove(struct dentry *dir);

void lsv_stats_set_enabled(bool on);

#else /* !LSV_STATS */

static inline bool lsv_stats_enabled(void)
{
	return false;
}

static inline void lsv_stats_inc(struct lsv_stats *stats, enum lsv_stat_cnt cnt)
{
}

static inline u64 lsv_stats_now(void)
{
	return 0;
}

static inline void lsv_stats_record(struct lsv_stats *stats, enum lsv_stat_lat lat, u64 ns)
{
}

static inline void lsv_stats_since(struct lsv_stats *stats, enum lsv_stat_lat lat, u64 start)
{
}

static inline s32 lsv_stats_init(struct lsv_stats *stats)
{
	return 0;
}

static inline void lsv_stats_free(struct lsv_stats *stats)
{
}

static inline s32 lsv_stats_debugfs_init(void)
{
	return 0;
}

static inline void lsv_stats_debugfs_deinit(void)
{
}

static inline struct dentry *lsv_stats_debugfs_add(struct lsv_dev *dev)
{
	return NULL;
}

static inline void lsv_stats_debugfs_remove(struct dentry *dir)
{
}

static inline void lsv_stats_set_enabled(bool on)
{
}

#endif /* LSV_STATS */

#endif
