// SPDX-License-Identifier: GPL-2.0-only

#include "linux/mempool.h"
#include <linux/slab.h>
#include <string.h>

#include "core/req.h"

#define LSV_MIN_REQ_NUM 128

struct lsv_req_engine {
	struct kmem_cache *req_cachep;
	mempool_t *req_pool;
};

static struct lsv_req_engine *g_req_engine;

s32 lsv_g_req_alloc(void)
{
	struct lsv_req_engine *engine = g_req_engine;

	engine->req_cachep = kmem_cache_create("lsv_req", sizeof(struct lsv_req), 0, SLAB_HWCACHE_ALIGN, NULL);
	if (!engine->req_cachep)
		return -ENOMEM;

	engine->req_pool = mempool_init_slab_pool(LSV_MIN_REQ_NUM, engine->req_cachep);

	return 0;
}

void lsv_g_req_free(void)
{
	struct lsv_req_engine *engine = g_req_engine;

	kmem_cache_destroy(engine->req_cachep);
	engine->req_cachep = NULL;

	mempool_destroy(engine->req_pool);
	engine->req_pool = NULL;
}

struct lsv_req *lsv_req_alloc(struct bio *bio, u64 lba, u32 offset, u32 sectors)
{
	struct lsv_req_engine *engine = g_req_engine;

	struct lsv_req *req;

	req = kmem_cache_zalloc(engine->req_cachep, GFP_NOIO);
	if (!req)
		return NULL;

	req->bio = bio;
	req->lba = lba;
	req->offset = offset;
	req->sectors = sectors;

	return req;
}

void lsv_req_free(struct lsv_req *req)
{
	struct lsv_req_engine *engine = g_req_engine;

	kmem_cache_free(engine->req_cachep, req);
}

void lsv_req_submit(struct lsv_req *req)
{
	struct bio *bio = req->bio;

	lsv_req_free(req);
	submit_bio_noacct(bio);
}

void lsv_req_end(struct lsv_req *req, blk_status_t status)
{
	struct bio *bio = req->bio;

	lsv_req_free(req);
	bio->bi_status = status;
	bio_endio(bio);
}
