/* SPDX-License-Identifier: GPL-2.0-only */

#include "ds_control.h"
#include "linux/err.h"
#include <linux/printk.h>
#include <linux/slab.h>

#include "dedup.h"
#include "core/bio.h"
#include "core/digest.h"

#define LSV_DEDUP_BUF_SIZE 4096

/*
 * Module wide state of the dedup layer, created once at module load and shared
 * by every device.
 */
struct lsv_dedup_engine {
	struct kmem_cache *data_buf_cachep;
	const struct lsv_digest_spec *digest_spec;
	struct lsv_dedup_map_cache dedup_cache;
};

static struct lsv_dedup_engine *g_dedup_engine;

s32 lsv_dedup_engine_init(void)
{
	struct lsv_dedup_engine *engine;

	engine = kzalloc(sizeof(*engine), GFP_KERNEL);
	if (!engine)
		return -ENOMEM;

	engine->digest_spec = lsv_digest_spec_get(LSV_DIGEST_XXH3_64);

	engine->data_buf_cachep = kmem_cache_create("lsv_dedup_data_buf", LSV_DEDUP_BUF_SIZE, 0, 0, NULL);
	if (!engine->data_buf_cachep) {
		kfree(engine);
		return -ENOMEM;
	}

	g_dedup_engine = engine;

	return 0;
}

void lsv_dedup_engine_deinit(void)
{
	struct lsv_dedup_engine *engine = g_dedup_engine;

	if (!engine)
		return;

	kmem_cache_destroy(engine->data_buf_cachep);

	g_dedup_engine = NULL;
	kfree(engine);
}

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
static u64 lsv_dedup_buf_digest_get(struct bio *bio, void *tmp_buf)
{
	u64 result = 0;

	lsv_bio_copy_to_buffer(bio, tmp_buf, LSV_DEDUP_BUF_SIZE);

	if (lsv_dedup_digest_calc(tmp_buf, &result, LSV_DEDUP_BUF_SIZE))
		return 0;

	if (!lsv_dedup_digest_is_usable(result)) {
		pr_info("digest %llu is reserved, falling back to a plain write\n", result);
		return 0;
	}

	return result;
}

static struct lsv_de *lsv_dedup_map_find_by_digest(struct lsv_dedup_map *map, u64 digest)
{
	return (struct lsv_de *)lsv_ds_lookup(map->ds, digest);
}

static int lsv_dedup_map_insert(struct lsv_dedup_map *map, u64 digest, struct lsv_de *de)
{
	struct lsv_dedup_map_cache *cache = &g_dedup_engine->dedup_cache;
	int rc;

	rc = lsv_ds_insert(map->ds, digest, de, cache);
	if (rc) {
		kmem_cache_free(cache->de_cachep, de);
		return rc;
	}

	return 0;
}

void lsv_dedup_remove_entry(struct lsv_dedup_map *map, struct lsv_de *de) {
	struct lsv_dedup_map_cache *cache = &g_dedup_engine->dedup_cache;

	lsv_ds_remove(map->ds, de->digest, cache);
}


struct lsv_de *lsv_dedup_process_digest(struct lsv_dedup *dedup, u64 digest)
{
	struct lsv_de_cache *cache = &g_dedup_engine->dedup_cache.de_cachep;
	struct lsv_dedup_map *map = dedup->map;
	struct lsv_de *de;
	int rc;

	/* TODO(qrutyy): change to atomic find or inc call */
	de = lsv_dedup_map_find_by_digest(map, digest);
	if (de) {
		atomic64_inc(&de->pblk->ref);
		return de;
	}

	de = kmem_cache_alloc(cache->de_cachep, GFP_NOIO);
	if (!de)
		return ERR_PTR(-ENOMEM);

	de->digest = digest;
	de->state = LSV_DE_NEW;

	rc = lsv_dedup_map_insert(map, digest, de);
	if (rc)
		return ERR_PTR(rc);
}

struct lsv_de *lsv_dedup_process(struct lsv_dedup *dedup, struct bio *bio)
{
	void *tmp_buf;
	u64 digest;

	tmp_buf = lsv_dedup_data_buf_alloc();
	digest = lsv_dedup_buf_digest_get(bio, tmp_buf);

	return lsv_dedup_process_digest(dedup, digest);
}
