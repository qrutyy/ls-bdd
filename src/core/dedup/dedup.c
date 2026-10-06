// SPDX-License-Identifier: GPL-2.0-only

#include <linux/err.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/workqueue.h>

#include "core/bio/io.h"
#include "core/dedup/dedup.h"
#include "core/dedup/digest.h"
#include "core/lmap/lmap.h"

/* Dedup works on whole cells, and only on cells of this size. */
#define LSV_DEDUP_BUF_SIZE 4096

/* Digests are spread uniformly, nothing gains from an ordered index. */
static char lsv_dedup_index_ds[] = "ht";

/* A write whose digest matched, kept until its bytes are compared with the stored cell's. */
struct lsv_dedup_verify {
	struct work_struct work;
	struct lsv_dedup *dedup;
	struct bio *bio;
	u64 lba;
	struct lsv_pblk *pblk; /* the candidate, one reference held */
	void *data; /* copy of the bio payload the digest was taken from */
};

/*
 * Module wide state of the dedup layer, created once at module load and shared
 * by every device.
 */
struct lsv_dedup_engine {
	struct kmem_cache *data_buf_cachep;
	struct kmem_cache *verify_cachep;
	/* Verify reads wait for the backing device, which ->submit_bio must not do. */
	struct workqueue_struct *verify_wq;
	const struct lsv_digest_spec *digest_spec;
	struct lsv_dedup_map_cache dedup_cache;
};

static struct lsv_dedup_engine *g_dedup_engine;

s32 lsv_dedup_engine_init(void)
{
	struct lsv_dedup_engine *engine;
	struct lsv_dedup_map_cache *cache;

	engine = kzalloc(sizeof(*engine), GFP_KERNEL);
	if (!engine)
		return -ENOMEM;

	engine->digest_spec = lsv_digest_spec_get(LSV_DIGEST_XXH3_64);

	/* Self aligned, so a buffer never straddles a page and suits any DMA alignment. */
	engine->data_buf_cachep = kmem_cache_create("lsv_dedup_data_buf", LSV_DEDUP_BUF_SIZE, LSV_DEDUP_BUF_SIZE,
						    0, NULL);
	if (!engine->data_buf_cachep)
		goto free_engine;

	engine->verify_cachep = kmem_cache_create("lsv_dedup_verify", sizeof(struct lsv_dedup_verify), 0, 0, NULL);
	if (!engine->verify_cachep)
		goto destroy_data_buf_cache;

	engine->verify_wq = alloc_workqueue("lsv_dedup_verify", WQ_MEM_RECLAIM | WQ_UNBOUND, 0);
	if (!engine->verify_wq)
		goto destroy_verify_cache;

	cache = &engine->dedup_cache;
	cache->de_cachep = kmem_cache_create("lsv_dedup_de", sizeof(struct lsv_de), 0, 0, NULL);
	if (!cache->de_cachep)
		goto destroy_verify_wq;

	cache->entry_cache_mng = kzalloc(sizeof(*cache->entry_cache_mng), GFP_KERNEL);
	if (!cache->entry_cache_mng)
		goto destroy_de_cache;

	g_dedup_engine = engine;

	return 0;

destroy_de_cache:
	kmem_cache_destroy(cache->de_cachep);
destroy_verify_wq:
	destroy_workqueue(engine->verify_wq);
destroy_verify_cache:
	kmem_cache_destroy(engine->verify_cachep);
destroy_data_buf_cache:
	kmem_cache_destroy(engine->data_buf_cachep);
free_engine:
	kfree(engine);
	return -ENOMEM;
}

void lsv_dedup_engine_deinit(void)
{
	struct lsv_dedup_engine *engine = g_dedup_engine;
	struct lsv_dedup_map_cache *cache;
	struct lsv_cache_mng *mng;

	if (!engine)
		return;

	cache = &engine->dedup_cache;
	mng = cache->entry_cache_mng;

	destroy_workqueue(engine->verify_wq);

	/* The index node caches are created lazily by the first device using them. */
	kmem_cache_destroy(mng->ht_cache);
	kmem_cache_destroy(mng->sl_cache);
	kmem_cache_destroy(mng->bt_cache);
	kmem_cache_destroy(mng->rb_cache);
	kfree(mng);

	kmem_cache_destroy(cache->de_cachep);
	kmem_cache_destroy(engine->verify_cachep);
	kmem_cache_destroy(engine->data_buf_cachep);

	g_dedup_engine = NULL;
	kfree(engine);
}

/*
 * ds_control takes its caches as a struct lsv_lmap_cache, named after its first
 * user but meaning "node caches plus the cache values are freed to". This is
 * that pair for the dedup index, whose values are the entries.
 */
static void lsv_dedup_ds_cache(struct lsv_lmap_cache *cache)
{
	cache->entry_cache_mng = g_dedup_engine->dedup_cache.entry_cache_mng;
	cache->cell_cachep = g_dedup_engine->dedup_cache.de_cachep;
}

struct lsv_dedup *lsv_dedup_create(struct lsv_lmap *lmap, struct block_device *bd)
{
	struct lsv_dedup *dedup;
	s32 rc = -ENOMEM;

	if (lmap->cell_size != LSV_DEDUP_BUF_SIZE) {
		pr_err("lsv: dedup needs %u byte cells, got %u\n", LSV_DEDUP_BUF_SIZE, lmap->cell_size);
		return ERR_PTR(-EINVAL);
	}

	dedup = kzalloc(sizeof(*dedup), GFP_KERNEL);
	if (!dedup)
		return ERR_PTR(-ENOMEM);

	dedup->map = kzalloc(sizeof(*dedup->map), GFP_KERNEL);
	if (!dedup->map)
		goto free_dedup;

	dedup->map->ds = kzalloc(sizeof(*dedup->map->ds), GFP_KERNEL);
	if (!dedup->map->ds)
		goto free_map;

	rc = lsv_ds_init(dedup->map->ds, lsv_dedup_index_ds, g_dedup_engine->dedup_cache.entry_cache_mng);
	if (rc)
		goto free_ds;

	dedup->lmap = lmap;
	dedup->bd = bd;

	return dedup;

free_ds:
	kfree(dedup->map->ds);
free_map:
	kfree(dedup->map);
free_dedup:
	kfree(dedup);
	return ERR_PTR(rc);
}

/*
 * The device is quiesced: no new writes, only verifies that may still be queued.
 * Freeing the index frees every entry, withdrawn ones included. The pblks are
 * left pointing at them, which is fine: lmap frees them next without looking.
 */
void lsv_dedup_destroy(struct lsv_dedup *dedup)
{
	struct lsv_lmap_cache cache;

	if (!dedup)
		return;

	flush_workqueue(g_dedup_engine->verify_wq);

	lsv_dedup_ds_cache(&cache);
	lsv_ds_free(dedup->map->ds, &cache);

	kfree(dedup->map->ds);
	kfree(dedup->map);
	kfree(dedup);
}

static bool lsv_dedup_digest_is_usable(u64 digest)
{
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

static void lsv_dedup_data_buf_free(void *buf)
{
	kmem_cache_free(g_dedup_engine->data_buf_cachep, buf);
}

static int lsv_dedup_digest_calc(void *data, void *result, size_t len)
{
	const struct lsv_dedup_engine *engine = g_dedup_engine;
	int rc;

	rc = lsv_digest_calculate(engine->digest_spec, data, len, result);
	if (rc)
		pr_info("failed to calculate a digest\n");

	return rc;
}

/*
 * Copy the granule this request carries into |tmp_buf| and digest it. The copy
 * is what a byte verify later compares against, so it is taken once, here, and
 * kept for as long as a verify may need it. Returns 0 when there is no usable
 * digest, which is free to mean that because 0 is a reserved key anyway.
 */
static u64 lsv_dedup_buf_digest_get(struct bio *bio, void *tmp_buf)
{
	u64 result = 0;

	if (lsv_bio_copy_to_buffer(bio, tmp_buf, LSV_DEDUP_BUF_SIZE) != LSV_DEDUP_BUF_SIZE)
		return 0;

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

/*
 * True if |de| made it into the index. The hashtable refuses a key it already
 * holds and frees the refused value itself, while lsv_ds_insert() returns 0
 * either way; so the entry now found under |digest| tells: it is either |de|,
 * or the one that was there first. A refused |de| must not be touched again.
 */
static bool lsv_dedup_map_insert(struct lsv_dedup_map *map, u64 digest, struct lsv_de *de)
{
	struct lsv_lmap_cache cache;

	lsv_dedup_ds_cache(&cache);

	if (lsv_ds_insert(map->ds, digest, de, &cache))
		return false;

	return lsv_dedup_map_find_by_digest(map, digest) == de;
}

/*
 * Only marks the entry removed: the hashtable keeps removed nodes, values
 * included, until it is freed. That is also what makes it safe for a concurrent
 * lookup to still be looking at |de|.
 */
static void lsv_dedup_remove_entry(struct lsv_dedup_map *map, struct lsv_de *de)
{
	lsv_ds_remove(map->ds, de->digest, g_dedup_engine->dedup_cache.de_cachep);
}

/* A reference on the pblk indexed under |digest|, or NULL if there is none. */
static struct lsv_pblk *lsv_dedup_lookup(struct lsv_dedup *dedup, u64 digest)
{
	struct lsv_pblk *pblk = NULL;
	struct lsv_de *de;

	/* The entry outlives the lookup, but its pblk may die meanwhile; RCU keeps it readable. */
	rcu_read_lock();

	de = lsv_dedup_map_find_by_digest(dedup->map, digest);

	if (de && refcount_inc_not_zero(&de->pblk->ref))
		pblk = de->pblk;

	rcu_read_unlock();

	return pblk;
}

/*
 * Data not seen before: a fresh pblk, indexed under |digest|. Entry and pblk
 * are linked before the entry is published, so whoever finds the entry finds a
 * complete pblk behind it.
 */
static struct lsv_pblk *lsv_dedup_insert(struct lsv_dedup *dedup, u64 digest)
{
	struct lsv_pblk *pblk;
	struct lsv_de *de;

	pblk = lsv_pblk_alloc(dedup->lmap);
	if (IS_ERR(pblk))
		return pblk;

	de = kmem_cache_alloc(g_dedup_engine->dedup_cache.de_cachep, GFP_NOIO);
	if (!de)
		return pblk;

	de->digest = digest;
	de->pblk = pblk;
	de->dedup = dedup;
	pblk->de = de;

	/* in case of insert failure de is unindexed */
	if (!lsv_dedup_map_insert(dedup->map, digest, de))
		pblk->de = NULL;

	return pblk;
}

/*
 * Matching digests only say the data is probably stored already. The stored cell
 * is read back and compared byte for byte; the bio is completed without any
 * write on a match, and written out as a block of its own otherwise.
 */
static void lsv_dedup_verify_fn(struct work_struct *work)
{
	struct lsv_dedup_verify *vw = container_of(work, struct lsv_dedup_verify, work);
	struct lsv_dedup *dedup = vw->dedup;
	struct lsv_lmap *lmap = dedup->lmap;
	struct lsv_pblk *pblk = vw->pblk;
	struct bio *bio = vw->bio;
	bool same = false;
	void *stored;
	u64 pba;
	s32 rc;

	stored = lsv_dedup_data_buf_alloc();
	if (stored) {
		pba = READ_ONCE(pblk->pba);
		rc = lsv_io_read(dedup->bd, lsv_lmap_data_sector(lmap, pba), stored, LSV_DEDUP_BUF_SIZE);
		same = !rc && !memcmp(vw->data, stored, LSV_DEDUP_BUF_SIZE);
		lsv_dedup_data_buf_free(stored);
	}

	if (same) {
		rc = lsv_lmap_bind(lmap, vw->lba, pblk);
		if (rc)
			goto put_pblk;

		bio_endio(bio);
		goto out;
	}

	/*
	 * Same digest, different bytes: a genuine collision, or the stored cell is
	 * not on disk (yet): its write is still in flight, or failed. The entry
	 * keeps describing the cell it was made for, and this data goes to a new,
	 * unindexed one.
	 */
	/* TODO(qrutyy): add DE_PROCESSED state for on disk guarantee */
	lsv_pblk_put(lmap, pblk);

	pblk = lsv_pblk_alloc(lmap);
	if (IS_ERR(pblk)) {
		rc = PTR_ERR(pblk);
		goto fail;
	}

	/* Before binding: from then on a concurrent overwrite may free the pblk. */
	pba = pblk->pba;

	rc = lsv_lmap_bind(lmap, vw->lba, pblk);
	if (rc)
		goto put_pblk;

	bio->bi_iter.bi_sector = lsv_lmap_data_sector(lmap, pba);
	submit_bio_noacct(bio);
	goto out;

put_pblk:
	lsv_pblk_put(lmap, pblk);
fail:
	bio->bi_status = errno_to_blk_status(rc);
	bio_endio(bio);
out:
	lsv_dedup_data_buf_free(vw->data);
	kmem_cache_free(g_dedup_engine->verify_cachep, vw);
}

/*
 * Runs in ->submit_bio and so never waits for I/O: the verify, which has to read
 * the backing device, is left to a worker.
 */
s32 lsv_dedup_write(struct lsv_dedup *dedup, struct bio *bio, u64 lba, struct lsv_pblk **pblk)
{
	struct lsv_dedup_verify *vw;
	struct lsv_pblk *found;
	void *tmp_buf;
	u64 digest;

	tmp_buf = lsv_dedup_data_buf_alloc();
	if (!tmp_buf)
		goto plain;

	digest = lsv_dedup_buf_digest_get(bio, tmp_buf);
	if (!digest)
		goto free_buf;

	found = lsv_dedup_lookup(dedup, digest);
	if (!found) {
		lsv_dedup_data_buf_free(tmp_buf);
		*pblk = lsv_dedup_insert(dedup, digest);
		return IS_ERR(*pblk) ? PTR_ERR(*pblk) : LSV_DE_NEW;
	}

	/* TODO(qrutyy): move in sep function */
	vw = kmem_cache_alloc(g_dedup_engine->verify_cachep, GFP_NOIO);
	if (!vw) {
		lsv_pblk_put(dedup->lmap, found);
		goto free_buf;
	}

	INIT_WORK(&vw->work, lsv_dedup_verify_fn);
	vw->dedup = dedup;
	vw->bio = bio;
	vw->lba = lba;
	vw->pblk = found;
	vw->data = tmp_buf;

	queue_work(g_dedup_engine->verify_wq, &vw->work);

	return LSV_DE_DUPLICATE;

free_buf:
	lsv_dedup_data_buf_free(tmp_buf);
plain:
	/* Dedup could not look at this write, but it still has to be stored. */
	*pblk = lsv_pblk_alloc(dedup->lmap);
	return IS_ERR(*pblk) ? PTR_ERR(*pblk) : LSV_DE_NEW;
}

/*
 * Called by lmap once the last cell dropped |de|'s pblk: the data it indexes is
 * garbage now, and must not be found by content any more.
 */
void lsv_dedup_forget(struct lsv_de *de)
{
	lsv_dedup_remove_entry(de->dedup->map, de);
}
