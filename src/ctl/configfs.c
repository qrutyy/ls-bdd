/* SPDX-License-Identifier: GPL-2.0-only */

#include <linux/blkdev.h>
#include <linux/configfs.h>
#include <linux/ctype.h>
#include <linux/log2.h>
#include <linux/kernel.h>
#include <linux/limits.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "../core/dev.h"
#include "../utils/ds_control.h"
#include "configfs.h"

/*
 * Device creation protocol:
 *
 *	mkdir /sys/kernel/config/lsv/lsv0
 *	echo /dev/nvme0n1 > /sys/kernel/config/lsv/lsv0/backing_path
 *	echo ht		  > /sys/kernel/config/lsv/lsv0/index_ds
 *	echo 4096	  > /sys/kernel/config/lsv/lsv0/cell_size
 *	echo 2097152	  > /sys/kernel/config/lsv/lsv0/segment_size
 *	echo 1		  > /sys/kernel/config/lsv/lsv0/create_new
 *
 * mkdir only allocates a description of a future device. The device itself
 * is instantiated by writing to create_new, once every attribute is set.
 * Attributes are immutable after that; rmdir tears the description down.
 */

static inline struct lsv_cfg_dev *to_lsv_cfg_dev(struct config_item *item)
{
	return container_of(item, struct lsv_cfg_dev, item);
}

/*
 * Copies a userspace-written value into a fixed buffer, trimming the trailing
 * newline that echo appends. Rejects writes once the device was instantiated.
 */
static ssize_t lsv_cfg_store_str(struct lsv_cfg_dev *cfg, char *dst, size_t dst_size, const char *page, size_t count)
{
	ssize_t rc = count;

	if (!count || count >= dst_size)
		return -EINVAL;

	mutex_lock(&cfg->lock);

	if (cfg->created) {
		rc = -EBUSY;
		goto out;
	}

	memcpy(dst, page, count);
	dst[count] = '\0';
	strim(dst);

	if (!dst[0])
		rc = -EINVAL;

out:
	mutex_unlock(&cfg->lock);
	return rc;
}

static ssize_t lsv_cfg_dev_backing_path_show(struct config_item *item, char *page)
{
	struct lsv_cfg_dev *cfg = to_lsv_cfg_dev(item);
	ssize_t len;

	mutex_lock(&cfg->lock);
	len = snprintf(page, PAGE_SIZE, "%s\n", cfg->backing_path);
	mutex_unlock(&cfg->lock);

	return len;
}

static ssize_t lsv_cfg_dev_backing_path_store(struct config_item *item, const char *page, size_t count)
{
	struct lsv_cfg_dev *cfg = to_lsv_cfg_dev(item);

	return lsv_cfg_store_str(cfg, cfg->backing_path, sizeof(cfg->backing_path), page, count);
}
CONFIGFS_ATTR(lsv_cfg_dev_, backing_path);

static ssize_t lsv_cfg_dev_index_ds_show(struct config_item *item, char *page)
{
	struct lsv_cfg_dev *cfg = to_lsv_cfg_dev(item);
	ssize_t len;

	mutex_lock(&cfg->lock);
	len = snprintf(page, PAGE_SIZE, "%s\n", cfg->index_ds);
	mutex_unlock(&cfg->lock);

	return len;
}

static ssize_t lsv_cfg_dev_index_ds_store(struct config_item *item, const char *page, size_t count)
{
	struct lsv_cfg_dev *cfg = to_lsv_cfg_dev(item);
	char buf[LSV_CTL_MAX_DS_NAME_LEN + 1];
	char *name;

	if (!count || count >= sizeof(buf))
		return -EINVAL;

	memcpy(buf, page, count);
	buf[count] = '\0';
	name = strim(buf);

	if (!lsv_ds_check_available(name)) {
		pr_warn("lsv: unknown data structure '%s'\n", name);
		return -EINVAL;
	}

	return lsv_cfg_store_str(cfg, cfg->index_ds, sizeof(cfg->index_ds), name, strlen(name));
}
CONFIGFS_ATTR(lsv_cfg_dev_, index_ds);

static ssize_t lsv_cfg_dev_segment_size_show(struct config_item *item, char *page)
{
	struct lsv_cfg_dev *cfg = to_lsv_cfg_dev(item);
	ssize_t len;

	mutex_lock(&cfg->lock);
	len = snprintf(page, PAGE_SIZE, "%llu\n", cfg->segment_size);
	mutex_unlock(&cfg->lock);

	return len;
}

static ssize_t lsv_cfg_dev_segment_size_store(struct config_item *item, const char *page, size_t count)
{
	struct lsv_cfg_dev *cfg = to_lsv_cfg_dev(item);
	ssize_t rc = count;
	u64 value;

	if (kstrtou64(page, 0, &value) || !value)
		return -EINVAL;

	mutex_lock(&cfg->lock);

	if (cfg->created)
		rc = -EBUSY;
	else
		cfg->segment_size = value;

	mutex_unlock(&cfg->lock);
	return rc;
}
CONFIGFS_ATTR(lsv_cfg_dev_, segment_size);

static ssize_t lsv_cfg_dev_cell_size_show(struct config_item *item, char *page)
{
	struct lsv_cfg_dev *cfg = to_lsv_cfg_dev(item);
	ssize_t len;

	mutex_lock(&cfg->lock);
	len = snprintf(page, PAGE_SIZE, "%u\n", cfg->cell_size);
	mutex_unlock(&cfg->lock);

	return len;
}

static ssize_t lsv_cfg_dev_cell_size_store(struct config_item *item, const char *page, size_t count)
{
	struct lsv_cfg_dev *cfg = to_lsv_cfg_dev(item);
	ssize_t rc = count;
	u32 value;

	if (kstrtou32(page, 0, &value))
		return -EINVAL;

	if (value < LSV_CELL_SIZE_MIN || value > LSV_CELL_SIZE_MAX || !is_power_of_2(value) || (value & (SECTOR_SIZE - 1)))
		return -EINVAL;

	mutex_lock(&cfg->lock);

	if (cfg->created)
		rc = -EBUSY;
	else
		cfg->cell_size = value;

	mutex_unlock(&cfg->lock);
	return rc;
}
CONFIGFS_ATTR(lsv_cfg_dev_, cell_size);

static ssize_t lsv_cfg_dev_create_new_show(struct config_item *item, char *page)
{
	struct lsv_cfg_dev *cfg = to_lsv_cfg_dev(item);

	return snprintf(page, PAGE_SIZE, "%d\n", READ_ONCE(cfg->created) ? 1 : 0);
}

static ssize_t lsv_cfg_dev_create_new_store(struct config_item *item, const char *page, size_t count)
{
	struct lsv_cfg_dev *cfg = to_lsv_cfg_dev(item);
	struct lsv_dev_params params;
	ssize_t rc = count;
	bool trigger;

	if (kstrtobool(page, &trigger))
		return -EINVAL;

	/* Teardown is done with rmdir, not by writing 0 here. */
	if (!trigger)
		return -EINVAL;

	mutex_lock(&cfg->lock);

	if (cfg->created) {
		rc = -EEXIST;
		goto out;
	}

	if (!cfg->backing_path[0]) {
		rc = -EINVAL;
		goto out;
	}

	if (!strlen(cfg->index_ds)) {
		rc = -EINVAL;
		goto out;
	}

	if (!cfg->cell_size) {
		rc = -EINVAL;
		goto out;
	}

	/*
	 * The configfs directory name is the device identity, so it is handed
	 * to the core explicitly. Everything the core needs travels in a plain
	 * parameter struct: core must not know about configfs.
	 */
	params.name = config_item_name(&cfg->item);
	params.back_path = cfg->backing_path;
	params.index_ds = cfg->index_ds;
	params.cell_size = cfg->cell_size;
	params.segment_size = cfg->segment_size;

	rc = lsv_dev_create(&params, &cfg->dev);
	if (rc)
		goto out;

	cfg->created = true;
	rc = count;

out:
	mutex_unlock(&cfg->lock);
	return rc;
}
CONFIGFS_ATTR(lsv_cfg_dev_, create_new);

static struct configfs_attribute *lsv_cfg_dev_attrs[] = {
	&lsv_cfg_dev_attr_backing_path,
	&lsv_cfg_dev_attr_index_ds,
	&lsv_cfg_dev_attr_cell_size,
	&lsv_cfg_dev_attr_segment_size,
	&lsv_cfg_dev_attr_create_new,
	NULL,
};

static void lsv_cfg_dev_release(struct config_item *item)
{
	struct lsv_cfg_dev *cfg = to_lsv_cfg_dev(item);

	if (cfg->created)
		lsv_dev_destroy(cfg->dev);

	mutex_destroy(&cfg->lock);
	kfree(cfg);
}

static struct configfs_item_operations lsv_cfg_dev_item_ops = {
	.release = lsv_cfg_dev_release,
};

static const struct config_item_type lsv_cfg_dev_type = {
	.ct_item_ops = &lsv_cfg_dev_item_ops,
	.ct_attrs = lsv_cfg_dev_attrs,
	.ct_owner = THIS_MODULE,
};

static struct config_item *lsv_cfg_make_item(struct config_group *group, const char *name)
{
	struct lsv_cfg_dev *cfg;

	size_t i;

	/* The name becomes gendisk->disk_name, so validate it here. */
	if (strlen(name) > LSV_CTL_MAX_BD_NAME_LENGTH || strlen(name) >= DISK_NAME_LEN)
		return ERR_PTR(-ENAMETOOLONG);

	for (i = 0; name[i]; i++)
		if (!isalnum(name[i]) && name[i] != '-' && name[i] != '_')
			return ERR_PTR(-EINVAL);

	cfg = kzalloc(sizeof(*cfg), GFP_KERNEL);
	if (!cfg)
		return ERR_PTR(-ENOMEM);

	mutex_init(&cfg->lock);
	config_item_init_type_name(&cfg->item, name, &lsv_cfg_dev_type);

	return &cfg->item;
}

static struct configfs_group_operations lsv_cfg_group_ops = {
	.make_item = lsv_cfg_make_item,
};

static const struct config_item_type lsv_cfg_subsys_type = {
	.ct_group_ops = &lsv_cfg_group_ops,
	.ct_owner = THIS_MODULE,
};

static struct configfs_subsystem lsv_cfg_subsys = {
	.su_group = {
		.cg_item = {
			.ci_namebuf = "lsv",
			.ci_type = &lsv_cfg_subsys_type,
		},
	},
};

s32 lsv_configfs_register(void)
{
	s32 rc;

	config_group_init(&lsv_cfg_subsys.su_group);
	mutex_init(&lsv_cfg_subsys.su_mutex);

	rc = configfs_register_subsystem(&lsv_cfg_subsys);
	if (rc)
		pr_err("lsv: configfs registration failed: %d\n", rc);

	return rc;
}

void lsv_configfs_unregister(void)
{
	configfs_unregister_subsystem(&lsv_cfg_subsys);
}
