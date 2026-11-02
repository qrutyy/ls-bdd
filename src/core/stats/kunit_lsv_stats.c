// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>
#include <linux/limits.h>

#include "core/stats/stats.h"
#include "utils/kunit.h"

/*
 * The bucket arithmetic every reported percentile rests on: buckets must be
 * ordered, cover every value, and stay within 1/16 of the values they hold.
 */

static void LSV_KTEST(stats_small_values_are_exact)(struct kunit *test)
{
	u64 v;

	for (v = 0; v < LSV_STATS_HIST_SUB; v++) {
		KUNIT_EXPECT_EQ(test, lsv_stats_bucket(v), (u32)v);
		KUNIT_EXPECT_EQ(test, lsv_stats_bucket_floor(v), v);
	}
}

static void LSV_KTEST(stats_floor_round_trips)(struct kunit *test)
{
	u32 b;

	for (b = 0; b < LSV_STATS_HIST_BUCKETS; b++)
		KUNIT_EXPECT_EQ(test, lsv_stats_bucket(lsv_stats_bucket_floor(b)), b);
}

static void LSV_KTEST(stats_floors_increase)(struct kunit *test)
{
	u32 b;

	for (b = 1; b < LSV_STATS_HIST_BUCKETS; b++)
		KUNIT_EXPECT_LT(test, lsv_stats_bucket_floor(b - 1), lsv_stats_bucket_floor(b));
}

/* Every value lands in a bucket whose floor is at most 1/16 below it. */
static void LSV_KTEST(stats_relative_error)(struct kunit *test)
{
	u64 v;

	for (v = 1; v < (1ULL << LSV_STATS_HIST_MAX_SHIFT); v = v * 9 / 8 + 1) {
		u32 b = lsv_stats_bucket(v);
		u64 floor = lsv_stats_bucket_floor(b);

		KUNIT_EXPECT_LE(test, floor, v);
		KUNIT_EXPECT_LE(test, v - floor, floor / LSV_STATS_HIST_SUB);
		if (b + 1 < LSV_STATS_HIST_BUCKETS)
			KUNIT_EXPECT_LT(test, v, lsv_stats_bucket_floor(b + 1));
	}
}

static void LSV_KTEST(stats_huge_values_share_the_last_bucket)(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, lsv_stats_bucket((1ULL << LSV_STATS_HIST_MAX_SHIFT) - 1), LSV_STATS_HIST_BUCKETS - 1);
	KUNIT_EXPECT_EQ(test, lsv_stats_bucket(1ULL << LSV_STATS_HIST_MAX_SHIFT), LSV_STATS_HIST_BUCKETS - 1);
	KUNIT_EXPECT_EQ(test, lsv_stats_bucket(U64_MAX), LSV_STATS_HIST_BUCKETS - 1);
}

static struct kunit_case __cases[] = {
	LSV_KUNIT_CASE(stats_small_values_are_exact),
	LSV_KUNIT_CASE(stats_floor_round_trips),
	LSV_KUNIT_CASE(stats_floors_increase),
	LSV_KUNIT_CASE(stats_relative_error),
	LSV_KUNIT_CASE(stats_huge_values_share_the_last_bucket),
	{}
};

static struct kunit_suite __suite = {
	.name = "lsv_stats",
	.test_cases = __cases,
};

kunit_test_suites(&__suite);
