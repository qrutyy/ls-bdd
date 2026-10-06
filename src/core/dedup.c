/* SPDX-License-Identifier: GPL-2.0-only */

#include "dedup.h"
#include "core/bio.h"
#include "linux/printk.h"

#define LSV_DEDUP_BUF_SIZE 4096

struct lsv_dedup_engine {
	struct kmem_cache *data_buf_cachep;

};

struct lsv_dedup_engine *g_dedup_engine;

static bool lsv_dedup_digest_is_usable(u64 digest) {
	return !lsv_ht_key_is_reserved(digest);
}

static void *lsv_dedup_data_buf_alloc(void)
{
	struct lsv_dedup_engine *engine = g_dedup_engine;
	void *buf;

	buf = kmem_cache_alloc(engine->data_buf_cachep, GFP_NOIO);
	if (!buf)
		pr_info("failed to allocate a data buffer\n");

	return buf;
}

static int lsv_dedup_digest_calc(void *data, void *result, size_t len)
{
	const struct lsv_dedup_engine *engine = g_dedup_engine; int rc;

	rc = lsv_digest_calculate(engine->digest_spec, data, len, result);
	if (rc)
		pr_info("failed to calculate a digest\n");

	return rc;
}

/*
 * Copy the granule this request carries into |tmp_buf| and digest it. The copy
 * is what a byte verify later compares against, so it is taken once, here, and
 * kept for as long as a verify may need it.
 */
static u64 lsv_dedup_buf_digest_get(struct lsv_req *req, void *tmp_buf)
{
	u64 result = 0;

	lsv_bio_copy_to_buffer(bio, tmp_buf, LSV_DEDUP_BUF_SIZE, true);

	if (lsv_dedup_digest_calc(tmp_buf, &result, LSV_DEDUP_BUF_SIZE))
		return 0;

	if (!lsv_dedup_digest_is_usable(result)) {
		pr_info("digest %llu is reserved, falling back to a plain write\n", result);
		return 0;
	}

	return result;
}

int lsv_dedup_process(struct bio *bio)
{
	lsv_dedup_data_buf_alloc();
	lsv_dedup_buf_digest_get()
	lsv_dedup_digest_calc

}
