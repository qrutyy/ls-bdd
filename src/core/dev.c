// SPDX-License-Identifier: GPL-2.0-only

#include <linux/blkdev.h>
#include <linux/err.h>
#include <linux/idr.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "core/bio/bio.h"
#include "core/dedup/dedup.h"
#include "core/dev.h"
#include "main.h"

static DEFINE_IDA(lsv_minor_ida);

static s32 lsv_dev_open_back(struct lsv_dev *dev, const char *path)
{
	struct file *bd_file;

	dev->back.path = kstrdup(path, GFP_KERNEL);
	if (!dev->back.path)
		return -ENOMEM;

	bd_file = bdev_file_open_by_path(path, BLK_OPEN_READ | BLK_OPEN_WRITE, NULL, NULL);
	if (IS_ERR(bd_file)) {
		kfree(dev->back.path);
		dev->back.path = NULL;
		return PTR_ERR(bd_file);
	}

	dev->back.bd_file = bd_file;
	dev->back.bd = file_bdev(bd_file);

	return 0;
}

static void lsv_dev_close_back(struct lsv_dev *dev)
{
	if (dev->back.bd_file) {
		fput(dev->back.bd_file);
		dev->back.bd_file = NULL;
		dev->back.bd = NULL;
	}

	kfree(dev->back.path);
	dev->back.path = NULL;
}

static s32 lsv_dev_add_disk(struct lsv_dev *dev)
{
	struct gendisk *disk;
	s32 minor;
	s32 rc;

	minor = ida_alloc_max(&lsv_minor_ida, LSV_MAX_MINORS - 1, GFP_KERNEL);
	if (minor < 0)
		return minor;

	disk = blk_alloc_disk(NULL, NUMA_NO_NODE);
	if (IS_ERR(disk)) {
		ida_free(&lsv_minor_ida, minor);
		return PTR_ERR(disk);
	}

	disk->major = g_mng->major;
	disk->first_minor = minor;
	disk->minors = 1;
	disk->fops = &lsv_bio_ops;
	disk->private_data = dev;
	strscpy(disk->disk_name, dev->front.name, sizeof(disk->disk_name));

	set_capacity(disk, get_capacity(dev->back.bd->bd_disk));

	rc = add_disk(disk);
	if (rc) {
		put_disk(disk);
		ida_free(&lsv_minor_ida, minor);
		return rc;
	}

	dev->front.disk = disk;
	dev->front.minor = minor;

	return 0;
}

static void lsv_dev_del_disk(struct lsv_dev *dev)
{
	if (!dev->front.disk)
		return;

	del_gendisk(dev->front.disk);
	put_disk(dev->front.disk);
	ida_free(&lsv_minor_ida, dev->front.minor);
	dev->front.disk = NULL;
}

s32 lsv_dev_create(const struct lsv_dev_params *params, struct lsv_dev **out)
{
	struct lsv_dev *dev;
	s32 rc;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	if (strscpy(dev->front.name, params->name, sizeof(dev->front.name)) < 0) {
		rc = -ENAMETOOLONG;
		goto free_dev;
	}

	rc = lsv_dev_open_back(dev, params->back_path);
	if (rc)
		goto free_dev;

	rc = lsv_lmap_init(&dev->lmap, params->index_ds, params->cell_size, params->segment_size,
			   get_capacity(dev->back.bd->bd_disk));
	if (rc)
		goto close_back;

	if (params->dedup) {
		dev->dedup = lsv_dedup_create(&dev->lmap, dev->back.bd);
		if (IS_ERR(dev->dedup)) {
			rc = PTR_ERR(dev->dedup);
			dev->dedup = NULL;
			goto deinit_lmap;
		}
	}

	rc = lsv_dev_add_disk(dev);
	if (rc)
		goto destroy_dedup;

	mutex_lock(&g_mng->lock);
	list_add_tail(&dev->node, &g_mng->dev_list);
	mutex_unlock(&g_mng->lock);

	*out = dev;

	pr_info("lsv: created '%s' on %s, cell %u B, %llu cells, dedup %s\n", dev->front.name, dev->back.path,
		dev->lmap.cell_size, dev->lmap.capacity_cells, dev->dedup ? "on" : "off");

	return 0;

destroy_dedup:
	lsv_dedup_destroy(dev->dedup);
deinit_lmap:
	lsv_lmap_deinit(&dev->lmap);
close_back:
	lsv_dev_close_back(dev);
free_dev:
	kfree(dev);
	return rc;
}

void lsv_dev_destroy(struct lsv_dev *dev)
{
	if (!dev)
		return;

	mutex_lock(&g_mng->lock);
	list_del(&dev->node);
	mutex_unlock(&g_mng->lock);

	lsv_dev_del_disk(dev);
	/* Before lmap: pending verifies still hold pblks and bios of this device. */
	lsv_dedup_destroy(dev->dedup);
	lsv_lmap_deinit(&dev->lmap);
	lsv_dev_close_back(dev);

	kfree(dev);
}
