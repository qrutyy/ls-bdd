// SPDX-License-Identifier: GPL-2.0-only

#include <linux/debugfs.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "core/dev.h"
#include "core/stats/stats.h"

/*
 * debugfs layout, one directory per device:
 *
 *	/sys/kernel/debug/lsv/enabled	  0/1, flips every probe at once
 *	/sys/kernel/debug/lsv/<dev>/stats  "name value" lines: counters, gauges
 *					  and a summary of every histogram
 *	/sys/kernel/debug/lsv/<dev>/hist	  "stage floor_ns count" for every
 *					  non-empty bucket, for offline analysis
 *	/sys/kernel/debug/lsv/<dev>/reset  any write zeroes the device's stats
 *
 * Reads sum the per-CPU copies without stopping writers, so a snapshot taken
 * under load is only approximately consistent across counters. Reset is meant
 * for an idle device: increments racing with it may survive it.
 */

DEFINE_STATIC_KEY_TRUE(lsv_stats_on);

static bool stats_enabled = true;
module_param(stats_enabled, bool, 0444);
MODULE_PARM_DESC(stats_enabled, "collect per-device stats from load on (toggle later in debugfs lsv/enabled)");

static const char *const lsv_stat_cnt_names[LSV_CNT_NR] = {
	[LSV_CNT_BIO_READ] = "bio_read",
	[LSV_CNT_BIO_WRITE] = "bio_write",
	[LSV_CNT_BIO_OTHER] = "bio_other",
	[LSV_CNT_BIO_ERROR] = "bio_error",
	[LSV_CNT_SPLIT] = "split",
	[LSV_CNT_READ_UNMAPPED] = "read_unmapped",
	[LSV_CNT_CELL_NEW] = "cell_new",
	[LSV_CNT_CELL_OVERWRITE] = "cell_overwrite",
	[LSV_CNT_PBLK_ALLOC] = "pblk_alloc",
	[LSV_CNT_PBLK_FREE] = "pblk_free",
	[LSV_CNT_ENOSPC] = "enospc",
	[LSV_CNT_PARTIAL_WRITE] = "partial_write",
	[LSV_CNT_DEDUP_NEW] = "dedup_new",
	[LSV_CNT_DEDUP_CANDIDATE] = "dedup_candidate",
	[LSV_CNT_DEDUP_HIT] = "dedup_hit",
	[LSV_CNT_DEDUP_COLLISION] = "dedup_collision",
	[LSV_CNT_DEDUP_SKIPPED] = "dedup_skipped",
};

static const char *const lsv_stat_lat_names[LSV_LAT_NR] = {
	[LSV_LAT_BIO] = "bio",
	[LSV_LAT_SUBMIT] = "submit",
	[LSV_LAT_REMAP_READ] = "remap_read",
	[LSV_LAT_REMAP_WRITE] = "remap_write",
	[LSV_LAT_IDX_LOOKUP] = "idx_lookup",
	[LSV_LAT_IDX_INSERT] = "idx_insert",
	[LSV_LAT_DEDUP_DIGEST] = "dedup_digest",
	[LSV_LAT_DEDUP_VERIFY] = "dedup_verify",
};

/* Percentiles in the summary, in per mille. */
static const u32 lsv_stats_permille[] = { 500, 900, 990, 999 };

static struct dentry *lsv_stats_root;

s32 lsv_stats_init(struct lsv_stats *stats)
{
	stats->pcpu = alloc_percpu(struct lsv_stats_cpu);

	return stats->pcpu ? 0 : -ENOMEM;
}

void lsv_stats_free(struct lsv_stats *stats)
{
	free_percpu(stats->pcpu);
	stats->pcpu = NULL;
}

void lsv_stats_set_enabled(bool on)
{
	if (on)
		static_branch_enable(&lsv_stats_on);
	else
		static_branch_disable(&lsv_stats_on);
}

static u64 lsv_stats_sum_cnt(struct lsv_stats *stats, enum lsv_stat_cnt cnt)
{
	u64 sum = 0;
	s32 cpu;

	for_each_possible_cpu(cpu)
		sum += per_cpu_ptr(stats->pcpu, cpu)->cnt[cnt];

	return sum;
}

static void lsv_stats_sum_hist(struct lsv_stats *stats, enum lsv_stat_lat lat, struct lsv_stats_hist *out)
{
	s32 cpu;
	u32 i;

	memset(out, 0, sizeof(*out));

	for_each_possible_cpu(cpu) {
		struct lsv_stats_hist *h = &per_cpu_ptr(stats->pcpu, cpu)->lat[lat];

		out->count += h->count;
		out->sum += h->sum;
		for (i = 0; i < LSV_STATS_HIST_BUCKETS; i++)
			out->buckets[i] += h->buckets[i];
	}
}

/*
 * The lower edge of the bucket holding the |permille|-th value. The count is
 * taken from the buckets rather than from |count|: both are summed while
 * writers run, and only the buckets agree with themselves.
 */
static u64 lsv_stats_hist_quantile(const struct lsv_stats_hist *h, u64 total, u32 permille)
{
	u64 rank = DIV_ROUND_UP_ULL(total * permille, 1000);
	u64 seen = 0;
	u32 i;

	for (i = 0; i < LSV_STATS_HIST_BUCKETS; i++) {
		seen += h->buckets[i];
		if (seen && seen >= rank)
			return lsv_stats_bucket_floor(i);
	}

	return 0;
}

static void lsv_stats_show_hist(struct seq_file *m, const char *name, const struct lsv_stats_hist *h)
{
	u64 total = 0;
	s32 last = -1;
	u32 i;

	for (i = 0; i < LSV_STATS_HIST_BUCKETS; i++) {
		total += h->buckets[i];
		if (h->buckets[i])
			last = i;
	}

	seq_printf(m, "lat_%s_count %llu\n", name, h->count);
	seq_printf(m, "lat_%s_mean_ns %llu\n", name, h->count ? div64_u64(h->sum, h->count) : 0);

	for (i = 0; i < ARRAY_SIZE(lsv_stats_permille); i++) {
		u32 pm = lsv_stats_permille[i];

		/* p500 -> p50, p990 -> p99, p999 -> p999 */
		if (pm % 10)
			seq_printf(m, "lat_%s_p%u_ns %llu\n", name, pm, lsv_stats_hist_quantile(h, total, pm));
		else
			seq_printf(m, "lat_%s_p%u_ns %llu\n", name, pm / 10, lsv_stats_hist_quantile(h, total, pm));
	}

	seq_printf(m, "lat_%s_max_ns %llu\n", name, last < 0 ? 0 : lsv_stats_bucket_floor(last));
}

static int lsv_stats_show(struct seq_file *m, void *unused)
{
	struct lsv_dev *dev = m->private;
	struct lsv_stats *stats = &dev->stats;
	struct lsv_stats_hist *h;
	u32 i;

	seq_printf(m, "enabled %d\n", lsv_stats_enabled());
	seq_printf(m, "cell_size %u\n", dev->lmap.cell_size);
	seq_printf(m, "capacity_cells %llu\n", dev->lmap.capacity_cells);
	seq_printf(m, "pba_next %lld\n", (s64)atomic64_read(&dev->lmap.next_pba));

	for (i = 0; i < LSV_CNT_NR; i++)
		seq_printf(m, "%s %llu\n", lsv_stat_cnt_names[i], lsv_stats_sum_cnt(stats, i));

	h = kmalloc(sizeof(*h), GFP_KERNEL);
	if (!h)
		return -ENOMEM;

	for (i = 0; i < LSV_LAT_NR; i++) {
		lsv_stats_sum_hist(stats, i, h);
		lsv_stats_show_hist(m, lsv_stat_lat_names[i], h);
	}

	kfree(h);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(lsv_stats);

static int lsv_stats_hist_show(struct seq_file *m, void *unused)
{
	struct lsv_dev *dev = m->private;
	struct lsv_stats_hist *h;
	u32 i, b;

	h = kmalloc(sizeof(*h), GFP_KERNEL);
	if (!h)
		return -ENOMEM;

	for (i = 0; i < LSV_LAT_NR; i++) {
		lsv_stats_sum_hist(&dev->stats, i, h);
		for (b = 0; b < LSV_STATS_HIST_BUCKETS; b++)
			if (h->buckets[b])
				seq_printf(m, "%s %llu %llu\n", lsv_stat_lat_names[i], lsv_stats_bucket_floor(b),
					   h->buckets[b]);
	}

	kfree(h);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(lsv_stats_hist);

static ssize_t lsv_stats_reset_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos)
{
	struct lsv_dev *dev = file->private_data;
	s32 cpu;

	for_each_possible_cpu(cpu)
		memset(per_cpu_ptr(dev->stats.pcpu, cpu), 0, sizeof(struct lsv_stats_cpu));

	return count;
}

static const struct file_operations lsv_stats_reset_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = lsv_stats_reset_write,
	.llseek = noop_llseek,
};

static ssize_t lsv_stats_enabled_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
	char val[3] = { lsv_stats_enabled() ? '1' : '0', '\n', '\0' };

	return simple_read_from_buffer(buf, count, ppos, val, 2);
}

static ssize_t lsv_stats_enabled_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos)
{
	bool on;
	s32 rc;

	rc = kstrtobool_from_user(buf, count, &on);
	if (rc)
		return rc;

	lsv_stats_set_enabled(on);

	return count;
}

static const struct file_operations lsv_stats_enabled_fops = {
	.owner = THIS_MODULE,
	.read = lsv_stats_enabled_read,
	.write = lsv_stats_enabled_write,
	.llseek = default_llseek,
};

s32 lsv_stats_debugfs_init(void)
{
	lsv_stats_set_enabled(stats_enabled);

	/* debugfs failures are not fatal, its calls take an ERR_PTR parent */
	lsv_stats_root = debugfs_create_dir("lsv", NULL);
	debugfs_create_file("enabled", 0600, lsv_stats_root, NULL, &lsv_stats_enabled_fops);

	return 0;
}

void lsv_stats_debugfs_deinit(void)
{
	debugfs_remove_recursive(lsv_stats_root);
	lsv_stats_root = NULL;
}

struct dentry *lsv_stats_debugfs_add(struct lsv_dev *dev)
{
	struct dentry *dir;

	dir = debugfs_create_dir(dev->front.name, lsv_stats_root);
	debugfs_create_file("stats", 0400, dir, dev, &lsv_stats_fops);
	debugfs_create_file("hist", 0400, dir, dev, &lsv_stats_hist_fops);
	debugfs_create_file("reset", 0200, dir, dev, &lsv_stats_reset_fops);

	return dir;
}

/* Waits for open readers, so |dev| may be freed once this returns. */
void lsv_stats_debugfs_remove(struct dentry *dir)
{
	debugfs_remove_recursive(dir);
}
