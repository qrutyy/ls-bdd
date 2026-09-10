// SPDX-License-Identifier: GPL-2.0-only

#include <linux/blkdev.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/slab.h>

#include "core/bio.h"
#include "core/dev.h"
#include "ctl/configfs.h"
#include "main.h"

MODULE_DESCRIPTION("Log-Structured Virtual block device driver module");
MODULE_AUTHOR("Mikhail Gavrilenko - @qrutyy");
MODULE_LICENSE("GPL v2");

struct lsv_mng *g_mng;

static s32 lsv_mng_init(void)
{
	struct lsv_mng *mng;
	s32 rc;

	mng = kzalloc(sizeof(*mng), GFP_KERNEL);
	if (!mng)
		return -ENOMEM;

	INIT_LIST_HEAD(&mng->dev_list);
	mutex_init(&mng->lock);

	rc = register_blkdev(0, LSV_BLKDEV_NAME_PREFIX);
	if (rc < 0) {
		pr_err("lsv: unable to register block device\n");
		goto free_mng;
	}
	mng->major = rc;

	rc = lsv_map_cache_alloc(&mng->map_cache);
	if (rc)
		goto unregister;

	rc = lsv_bio_cache_alloc();
	if (rc)
		goto free_map_cache;

	g_mng = mng;

	return 0;

free_map_cache:
	lsv_map_cache_free(&mng->map_cache);
unregister:
	unregister_blkdev(mng->major, LSV_BLKDEV_NAME_PREFIX);
free_mng:
	mutex_destroy(&mng->lock);
	kfree(mng);
	return rc;
}

static void lsv_dev_destroy_all(struct lsv_mng *mng)
{
	struct lsv_dev *entry;
	struct lsv_dev *tmp;

	list_for_each_entry_safe(entry, tmp, &mng->dev_list, node)
		lsv_dev_destroy(entry);
}

static void lsv_mng_deinit(void)
{
	struct lsv_mng *mng = g_mng;

	lsv_dev_destroy_all(mng);

	lsv_bio_cache_free();
	lsv_map_cache_free(&mng->map_cache);
	unregister_blkdev(mng->major, LSV_BLKDEV_NAME_PREFIX);

	g_mng = NULL;
	mutex_destroy(&mng->lock);
	kfree(mng);
}

static s32 __init lsv_init(void)
{
	s32 rc;

	rc = lsv_mng_init();
	if (rc)
		return rc;

	rc = lsv_configfs_register();
	if (rc) {
		lsv_mng_deinit();
		return rc;
	}

	pr_debug("lsv: module initialised\n");

	return 0;
}

static void __exit lsv_exit(void)
{
	lsv_configfs_unregister();
	lsv_mng_deinit();

	pr_debug("lsv: module exited\n");
}

module_init(lsv_init);
module_exit(lsv_exit);
