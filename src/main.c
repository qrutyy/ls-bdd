// SPDX-License-Identifier: GPL-2.0-only

#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/list.h>
#include <linux/moduleparam.h>
#include "utils/ds-control.h"
#include "utils/hashtable-utils.h"
#include "main.h"

MODULE_DESCRIPTION("Log-Structured virtual Block Device Driver module");
MODULE_AUTHOR("Mike Gavrilenko - @qrutyy");
MODULE_LICENSE("Dual MIT/GPL");

static s32  bdd_major;
char sel_ds[LSBDD_MAX_DS_NAME_LEN + 1];
struct bio_set *bdd_pool;
struct list_head bd_list;
atomic64_t next_free_sector = ATOMIC_INIT(LSBDD_SECTOR_OFFSET);

static struct kmem_cache *lsbdd_sectors_cache = NULL;
static struct kmem_cache *lsbdd_value_cache = NULL;
static struct kmem_cache *lsbdd_hash_cache = NULL;

static void vector_add_bd(struct bd_manager *current_bdev_manager)
{
	list_add_tail(&current_bdev_manager->list, &bd_list);
}

static struct bd_manager *get_bd_manager_by_name(char *vbd_name)
{
	struct bd_manager *entry = NULL;

	list_for_each_entry(entry, &bd_list, list) {
		if (!strcmp(entry->vbd_disk->disk_name, vbd_name))
			return entry;
	}

	return NULL;
}

static struct bd_manager *get_list_element_by_index(u16 index)
{
	struct bd_manager *entry = NULL;
	u16 i = 0;

	list_for_each_entry(entry, &bd_list, list) {
		if (i == index)
			return entry;
		i++;
	}

	return NULL;
}

static s8 convert_to_int(const char *arg, u8 *result)
{
	long number = 0;
	s32 res = kstrtol(arg, 10, &number);

	if (res != 0)
		return res;

	if (number < 0 || number > 255)
		return -ERANGE;

	*result = (u8)number;
	return 0;
}

static struct bdev_handle *open_bd_on_rw(char *bd_path)
{
	return bdev_open_by_path(bd_path, BLK_OPEN_WRITE | BLK_OPEN_READ, NULL,
							 NULL);
}

static void bdd_bio_end_io(struct bio *bio)
{
	bio_endio(bio->bi_private);
	bio_put(bio);
}

/**
 * Configures write operations in clone segments for the specified BIO.
 * Allocates memory for original and redirected sector data, retrieves the current
 * redirection info from the chosen data structure, and updates the mapping if necessary.
 * The redirected sector is then set in the clone BIO for processing.
 *
 * @main_bio - The original BIO representing the main device I/O operation.
 * @clone_bio - The clone BIO representing the redirected I/O operation.
 * @bd_manager - Manager that stores information about used ds and bdd in whole.
 *
 * It returns 0 on success, -ENOMEM if memory allocation fails.
 */
static s32 setup_write_in_clone_segments(struct bio *main_bio, struct bio *clone_bio, struct bd_manager *current_redirect_manager)
{
	s8 status;
	struct sectors *sectors = NULL;
	struct value_redir *old_value = NULL;
	struct value_redir *curr_value = NULL;

	// i guess this allocation can be deleted.... (if it will affect latency a lot)
	// btw it was made only for readability reason ;)
	sectors = kmem_cache_alloc(lsbdd_sectors_cache, GFP_KERNEL); 
	curr_value = kmem_cache_alloc(lsbdd_value_cache, GFP_KERNEL);

	if (!(sectors && curr_value))
		goto mem_err;

	sectors->original = main_bio->bi_iter.bi_sector;
	sectors->redirect = atomic64_read(&next_free_sector);

	pr_debug("Original sector: bi_sector = %llu, block_size %u\n",
			main_bio->bi_iter.bi_sector, clone_bio->bi_iter.bi_size);

	curr_value->block_size = main_bio->bi_iter.bi_size;
	curr_value->redirected_sector = sectors->redirect;

	old_value = ds_lookup(current_redirect_manager->sel_data_struct, sectors->original);

	pr_debug("WRITE: Old rs %p", old_value);
	pr_debug("WRITE: key: %llu, sec: %llu\n", sectors->original, curr_value->redirected_sector);

	if (old_value) {
		pr_debug("WRITE: remove old mapping key %lld old_val: %lld, new_val %lld\n", sectors->original, old_value->redirected_sector, sectors->redirect);
		ds_remove(current_redirect_manager->sel_data_struct, sectors->original);
	} else {
		atomic64_add(curr_value->block_size / SECTOR_SIZE, &next_free_sector);
	}

	status = ds_insert(current_redirect_manager->sel_data_struct, sectors->original, curr_value, lsbdd_hash_cache);
	if (status)
		goto insert_err;

	clone_bio->bi_iter.bi_sector = sectors->redirect;
	pr_debug("original %llu, redirected %llu\n", sectors->original, sectors->redirect);

	kmem_cache_free(lsbdd_sectors_cache, sectors);
	return 0;

insert_err:
	pr_err("Failed inserting key: %llu vallue: %p in _\n", sectors->original, curr_value);
	kmem_cache_free(lsbdd_sectors_cache, sectors);
	kmem_cache_free(lsbdd_value_cache, curr_value);
	return status;

mem_err:
pr_err("Memory allocation failed\n");
	kmem_cache_free(lsbdd_sectors_cache, sectors);
	kmem_cache_free(lsbdd_value_cache, curr_value);
	return -ENOMEM;
}

/**
 * Prepares a BIO split for partial handling of a clone BIO. Splits the clone BIO
 * s32 o two parts, so the first half (split_bio) can be processed independently.
 * This function submits the split_bio to be read separately from the remaining
 * data in clone_bio.
 *
 * @clone_bio - The clone BIO to be split.
 * @main_bio - The main BIO containing the primary I/O request data.
 * @nearest_bs - The block size in bytes closest to the current data segment.
 *
 * It returns nearest_bs on successful split, -1 if memory allocation fails.
 */
static s32 setup_bio_split(struct bio *clone_bio, struct bio *main_bio, s32 nearest_bs)
{
	struct bio *split_bio = NULL; // first half of splitted bio

	split_bio = bio_split(clone_bio, nearest_bs / SECTOR_SIZE, GFP_KERNEL, bdd_pool);
	if (!split_bio)
		return -1;

	pr_debug("RECURSIVE READ p1: bs = %u, main to read = %u, st sec = %llu\n",
		split_bio->bi_iter.bi_size, main_bio->bi_iter.bi_size, split_bio->bi_iter.bi_sector);
	pr_debug("RECURSIVE READ p2: bs = %u, main to read = %u,  st sec = %llu\n",
		clone_bio->bi_iter.bi_size, main_bio->bi_iter.bi_size, clone_bio->bi_iter.bi_sector);

	submit_bio(split_bio);
	pr_debug("Submitted bio first part of splitted main_bio\n\n");

	return nearest_bs;
}


/**
 * Identifies and handles system BIOs for the given BIO operation.
 *
 * This function checks whether the specified BIO corresponds to a system-level operation.
 * It determines this by inspecting the state of the selected data structure (DS) and
 * comparing the original sector to the redirection mappings stored in the DS.
 *
 * If the data structure is empty, or if the original sector is larger than the last
 * redirected sector, the BIO is marked as a system BIO by setting its sector to the
 * original value. Otherwise, the BIO is treated as a redirected operation.
 *
 * @redirect_manager: Manager holding information about the redirection state
 *                    and selected data structure.
 * @sectors: Sectors structure containing the original sector details.
 * @bio: BIO structure representing the I/O operation.
 *
 * Return:
 * - -1 if the BIO is identified as a system BIO.
 * - 0 if the BIO is redirected or otherwise successfully processed.
 */
static s16 check_system_bio(struct bd_manager *redirect_manager, struct sectors *sectors, struct bio *bio)
{
	struct value_redir *last_rs = NULL;

	if (ds_empty_check(redirect_manager->sel_data_struct)) {
		bio->bi_iter.bi_sector = sectors->original;
		return -1;
	}

	last_rs = ds_last(redirect_manager->sel_data_struct, sectors->original);
	pr_debug("READ: last_rs = %llu\n", last_rs->redirected_sector);

	if (sectors->original > last_rs->redirected_sector) {
		bio->bi_iter.bi_sector = sectors->original;
		pr_debug("Recognised system bio\n");
		return -1;
	}
	return 0;
}

/**
 * Configures read operations for clone segments based on redirection info from
 * the chosen data structure. This function retrieves the mapped or previous sector information,
 * determines the appropriate sector to read, and optionally splits the clone BIO
 * if more data is required. Handles cases where redirected and original sector
 * start points differ.
 *
 * @main_bio - The primary BIO representing the main device I/O operation.
 * @clone_bio - The clone BIO representing the redirected I/O operation.
 * @redirect_manager - Manages redirection data for mapped sectors.
 *
 * It returns 0 on success, -ENOMEM if memory allocation fails, or -1 on split error.
 */
static s32 setup_read_from_clone_segments(struct bio *main_bio, struct bio *clone_bio, struct bd_manager *redirect_manager)
{
	struct value_redir *curr_value = NULL;
	struct value_redir *prev_value = NULL;
	struct sectors *sectors = NULL;
	sector_t prev_sector_val = 0;
	sector_t *prev_sector = &prev_sector_val;
	s32 to_end_of_block = 0;
	s32 to_read_in_clone = 0;
	s16 status = 0;

	if (main_bio->bi_iter.bi_size == 0)
		return 0;

	sectors = kmem_cache_alloc(lsbdd_value_cache, GFP_KERNEL);
	if (!sectors)
		goto mem_err;

	sectors->original = main_bio->bi_iter.bi_sector;
	curr_value = ds_lookup(redirect_manager->sel_data_struct, sectors->original);

	pr_debug("READ: key: %llu\n", sectors->original);

	if (!curr_value) { // Read & Write sector starts aren't equal.
		status = check_system_bio(redirect_manager, sectors, clone_bio);
		if (status)
			return 0;

		pr_debug("READ: Sector: %llu isnt mapped\n", sectors->original);

		prev_value = ds_prev(redirect_manager->sel_data_struct, sectors->original, prev_sector);
		sectors->redirect = prev_value->redirected_sector * SECTOR_SIZE + (sectors->original - *prev_sector) * SECTOR_SIZE;
		to_end_of_block = (prev_value->redirected_sector * SECTOR_SIZE + prev_value->block_size) - sectors->redirect;
		to_read_in_clone = main_bio->bi_iter.bi_size - to_end_of_block;
		/* Address of main block end (reading from operation pba + bi_size) - End of previous block */

		clone_bio->bi_iter.bi_sector = prev_value->redirected_sector + (prev_value->block_size - to_end_of_block) / SECTOR_SIZE;

		pr_debug("To read = %d, to end = %d, main size = %u, prev_rs bs = %u, prev_rs sector = %llu\n", to_read_in_clone, to_end_of_block, main_bio->bi_iter.bi_size, prev_value->block_size, prev_value->redirected_sector);
		pr_debug("Clone bio: sector = %llu, size = %u\n", clone_bio->bi_iter.bi_sector, clone_bio->bi_iter.bi_size);

		if (to_read_in_clone < main_bio->bi_iter.bi_size && to_read_in_clone != 0) {
			while (to_end_of_block > 0) {
				status = setup_bio_split(clone_bio, main_bio, to_end_of_block);
				if (status < 0)
					goto split_err;

				if (to_read_in_clone > prev_value->block_size) {
					to_read_in_clone -= prev_value->block_size;
					to_end_of_block = prev_value->block_size;
				} else {
					break;
				}
			}
		}
		clone_bio->bi_iter.bi_size = (to_read_in_clone <= 0) ? to_end_of_block : to_read_in_clone;
	} else if (curr_value->redirected_sector) { // Read & Write start sectors are equal.
		pr_debug("Found redirected sector: %llu, rs_bs = %u, main_bs = %u\n",
			(curr_value->redirected_sector), curr_value->block_size, main_bio->bi_iter.bi_size);

		to_read_in_clone = main_bio->bi_iter.bi_size - curr_value->block_size;
		clone_bio->bi_iter.bi_sector = curr_value->redirected_sector;

		while (to_read_in_clone > 0) {
			to_read_in_clone -= setup_bio_split(clone_bio, main_bio, curr_value->block_size);
			if (status < 0)
				goto split_err;
		}
		clone_bio->bi_iter.bi_size = (to_read_in_clone < 0) ? curr_value->block_size + to_read_in_clone : curr_value->block_size;
		pr_debug("End of read, Clone: size: %u, sector %llu, to_read = %d\n", clone_bio->bi_iter.bi_size, clone_bio->bi_iter.bi_sector, to_read_in_clone);
	}
	return 0;

split_err:
	pr_err("Bio split went wrong\n");
	kmem_cache_free(lsbdd_sectors_cache, sectors);
	bio_io_error(main_bio);
	return -1;

mem_err:
	pr_err("Memory allocation failed.\n");
	kmem_cache_free(lsbdd_sectors_cache, sectors);
	return -ENOMEM;
}

/**
 * lsbdd_submit_bio() - Takes the provided bio, allocates a clone (child)
 * for a redirect_bd. Although, it changes the way both bio's will end (+ maps
 * bio address with free one from aim BD in chosen data structure) and submits them.
 *
 * @bio - Expected bio request
 */
static void lsbdd_submit_bio(struct bio *bio)
{
	struct bio *clone = NULL;
	struct bd_manager *current_redirect_manager = NULL;
	s16 status;

	pr_debug("Entered submit bio\n");

	current_redirect_manager = get_bd_manager_by_name(bio->bi_bdev->bd_disk->disk_name);
	if (!current_redirect_manager)
		goto get_err;

	clone = bio_alloc_clone(current_redirect_manager->bd_handler->bdev, bio,
							GFP_KERNEL, bdd_pool);
	if (!clone)
		goto clone_err;

	clone->bi_private = bio;
	clone->bi_end_io = bdd_bio_end_io;

	if (bio_op(bio) == REQ_OP_READ)
		status = setup_read_from_clone_segments(bio, clone, current_redirect_manager);
	else if (bio_op(bio) == REQ_OP_WRITE)
		status = setup_write_in_clone_segments(bio, clone, current_redirect_manager);
	else
		pr_warn("Unknown Operation in bio\n");


	if (status)
		goto setup_err;


	submit_bio(clone);
	pr_debug("Submitted bio\n\n");
	return;

get_err:
	pr_err("No such bd_manager with middle disk %s and not empty handler\n",
		bio->bi_bdev->bd_disk->disk_name);
	bdd_bio_end_io(bio);
	return;

clone_err:
	pr_err("Bio allocation failed\n");
	bio_io_error(bio);
	return;

setup_err:
	pr_err("Setup failed with code %d\n", status);
	bio_io_error(bio);
	return;
}

static const struct block_device_operations lsbdd_bio_ops = {
	.owner = THIS_MODULE,
	.submit_bio = lsbdd_submit_bio,
};

/**
 * init_disk_bd() - Initialises gendisk structure, for 'middle' disk
 * @vbd_name: name of creating BD
 *
 * DOESN'T SET UP the disks capacity, check lsbdd_submit_bio()
 * AND DOESN'T ADD disk if there was one already TO FIX?
 */
static struct gendisk *init_disk_bd(char *vbd_name)
{
	struct gendisk *new_disk = NULL;
	struct bd_manager *linked_manager = NULL;

	new_disk = blk_alloc_disk(NUMA_NO_NODE);

	new_disk->major = bdd_major;
	new_disk->first_minor = 1;
	new_disk->minors = LSBDD_MAX_MINORS_AM;
	new_disk->fops = &lsbdd_bio_ops;

	if (vbd_name) {
		strcpy(new_disk->disk_name, vbd_name);
	} else {
		pr_warn("vbd_name is NULL, nothing to copy\n");
		return NULL;
	}

	if (list_empty(&bd_list)) {
		pr_warn("Couldn't init disk, bc list is empty\n");
		return NULL;
	}

	linked_manager = list_last_entry(&bd_list, struct bd_manager, list);
	set_capacity(new_disk,
				 get_capacity(linked_manager->bd_handler->bdev->bd_disk));
	return new_disk;
}

/**
 * check_and_open_bd() - Checks if name is occupied, if so - opens the BD, if
 * not - return -EINVAL. Additionally adds the BD to the vector.
 * and initialises data_struct.
 */
static s32 check_and_open_bd(char *bd_path)
{
	struct bd_manager *current_bdev_manager = kzalloc(sizeof(struct bd_manager), GFP_KERNEL);
	struct bdev_handle *current_bdev_handle = NULL;
	struct data_struct *curr_ds = kzalloc(sizeof(struct data_struct), GFP_KERNEL);

	if (!curr_ds + !current_bdev_manager > 0)
		goto mem_err;

	current_bdev_handle = open_bd_on_rw(bd_path);

	if (IS_ERR(current_bdev_handle))
		goto free_bdev;

	current_bdev_manager->bd_handler = current_bdev_handle;
	current_bdev_manager->vbd_name = bd_path;
	current_bdev_manager->sel_data_struct = curr_ds;

	vector_add_bd(current_bdev_manager);

	pr_debug("Succesfully added %s to vector\n", bd_path);

	return 0;

free_bdev:
	pr_err("Couldnt open bd by path: %s\n", bd_path);
	kfree(curr_ds);
	kfree(current_bdev_manager);
	return PTR_ERR(current_bdev_handle);

mem_err:
	kfree(current_bdev_manager);
	kfree(curr_ds);
	return -ENOMEM;
}

static char *create_disk_name_by_index(s32 index)
{
	char *disk_name =
		kmalloc(strlen(LSBDD_BLKDEV_NAME_PREFIX) + snprintf(NULL, 0, "%d", index) + 1,
				GFP_KERNEL);

	if (disk_name != NULL)
		sprintf(disk_name, "%s%d", LSBDD_BLKDEV_NAME_PREFIX, index);

	return disk_name;
}

/**
 * Sets the name for a new BD, that will be used as 'device in the middle'.
 * Adds disk to the last bd_manager, that was modified by adding bd_handler
 * through check_and_open_bd()
 *
 * @name_index - an index for disk name
 *
 */
static s32 create_bd(s32 name_index)
{
	char *disk_name = NULL;
	s8 status;
	struct gendisk *new_disk = NULL;

	disk_name = create_disk_name_by_index(name_index);

	if (!disk_name)
		goto mem_err;

	new_disk = init_disk_bd(disk_name);

	if (!new_disk)
		goto disk_init_err;

	if (list_empty(&bd_list)) {
		pr_err("Couldn't init disk, bc list is empty\n");
		goto disk_init_err;
	}

	list_last_entry(&bd_list, struct bd_manager, list)->vbd_disk = new_disk;

	strcpy(list_last_entry(&bd_list, struct bd_manager, list)->vbd_disk->disk_name, disk_name);

	status = add_disk(new_disk);

	pr_debug("Status after add_disk with name %s: %d\n", disk_name, status);

	if (status) {
		put_disk(new_disk);
		goto disk_init_err;
	}

	return 0;

mem_err:
	pr_err("Memory allocation failed\n");
	kfree(disk_name);
	return -ENOMEM;

disk_init_err:
	pr_err("Disk initialization failed\n");
	kfree(new_disk);
	kfree(disk_name);
	return -ENOMEM;
}

static s8 delete_bd(u16 index)
{
	if (get_list_element_by_index(index)->bd_handler) {
		bdev_release(get_list_element_by_index(index)->bd_handler);
		get_list_element_by_index(index)->bd_handler = NULL;
	} else {
		pr_debug("BD with num %d is empty\n", index + 1);
	}
	if (get_list_element_by_index(index)->vbd_disk) {
		del_gendisk(get_list_element_by_index(index)->vbd_disk);
		put_disk(get_list_element_by_index(index)->vbd_disk);
		get_list_element_by_index(index)->vbd_disk = NULL;
	}
	if (get_list_element_by_index(index)->sel_data_struct) {
		ds_free(get_list_element_by_index(index)->sel_data_struct, lsbdd_hash_cache);
		get_list_element_by_index(index)->sel_data_struct = NULL;
	}

	list_del(&(get_list_element_by_index(index)->list));

	pr_debug("Removed bdev with index %d (from list)\n", index + 1);
	return 0;
}

/**
 * lsbdd_get_vbd_names() - Function that prints the list of block devices, that
 * are stored in vector.
 *
 * Vector stores only BD's that we've touched from this module.
 */
static s32 lsbdd_get_vbd_names(char *buf, const struct kernel_param *kp)
{
	struct bd_manager *current_manager = NULL;
	u8 total_length = 0;
	u8 offset = 0;
	u8 i = 0;
	u8 length = 0;

	if (list_empty(&bd_list)) {
		pr_warn("Vector is empty\n");
		return 0;
	}

	list_for_each_entry(current_manager, &bd_list, list) {
		if (current_manager->bd_handler != NULL) {
			i++;
			length = sprintf(
				buf + offset, "%d. %s -> %s\n", i,
				current_manager->vbd_disk->disk_name,
				current_manager->bd_handler->bdev->bd_disk->disk_name);

			if (length < 0) {
				pr_err("Error in formatting string\n");
				return -EFAULT;
			}

			offset += length;
			total_length += length;
		}
	}

	return total_length;
}

/**
 * lsbdd_delete_bd() - Deletes bdev according to index from printed list (check
 * lsbdd_get_vbd_names)
 */
static s32  lsbdd_delete_bd(const char *arg, const struct kernel_param *kp)
{
	u8 index = 0;
	s8 result = 0;

	result = convert_to_int(arg, &index);

	if (result) {
		pr_err("Block device index was entered not as s32\n");
		BUG();
	}

	delete_bd(index - 1);

	return 0;
}

static s32  check_available_ds(char *current_ds)
{
	u8 i = 0;
	u8 len = 0;

	len = ARRAY_SIZE(available_ds);

	for (i = 0; i < len; ++i) {
		if (!strcmp(available_ds[i], current_ds))
			return 0;
	}
	return -1;
}

static s32  lsbdd_get_data_structs(char *buf, const struct kernel_param *kp)
{
	u8 i = 0;
	u8 offset = 0;
	u8 length = 0;
	u8 total_length = 0;

	for (i = 0; i < ARRAY_SIZE(available_ds); i++) {
		length = sprintf(buf + offset, "%d. %s\n", i, available_ds[i]);

		if (length < 0) {
			pr_err("Error in formatting string\n");
			return -EFAULT;
		}

		offset += length;
		total_length += length;
	}

	return total_length;
}

/**
 * Function sets data structure that will be used for mapping of sectors
 * @arg - "type"
 */
static s32  lsbdd_set_data_struct(const char *arg, const struct kernel_param *kp)
{

	if (sscanf(arg, "%s", sel_ds) != 1) {
		pr_err("Wrong input, 1 vallue required\n");
		return -EINVAL;
	}

	if (check_available_ds(sel_ds)) {
		pr_err("%s is not supported. Check available data structure by set_data_structs\n", sel_ds);
		return -1;
	}
	return 0;
}

/**
 * Function links 'middle' BD and the aim one, for vector purposes. (creates,
 * opens and links)
 * @arg - "from_disk_postfix path"
 */
static s32  lsbdd_set_redirect_bd(const char *arg, const struct kernel_param *kp)
{
	s8 status;
	s32 index;
	char path[LSBDD_MAX_BD_NAME_LENGTH];

	if (sscanf(arg, "%d %s", &index, path) != 2) {
		pr_err("Wrong input, 2 values are required\n");
		return -EINVAL;
	}

	status = check_and_open_bd(path);

	if (!list_empty(&bd_list))
		bdd_major = register_blkdev(0, LSBDD_BLKDEV_NAME_PREFIX);

	if (status)
		return PTR_ERR(&status);

	status = ds_init(list_last_entry(&bd_list, struct bd_manager, list)->sel_data_struct, sel_ds);
	pr_debug("%p\n", list_last_entry(&bd_list, struct bd_manager, list)->sel_data_struct);
	if (status)
		return status;

	status = create_bd(index);

	if (status)
		return status;

	return 0;
}

static s32  __init lsbdd_init(void)
{
	s8 status;

	pr_debug("LSBDD module initialised\n");
	bdd_major = register_blkdev(0, LSBDD_BLKDEV_NAME_PREFIX);

	if (bdd_major < 0) {
		pr_err("Unable to register mybdev block device\n");
		BUG();
	}

	bdd_pool = kzalloc(sizeof(struct bio_set), GFP_KERNEL);

	if (!bdd_pool)
		goto mem_err;

	status = bioset_init(bdd_pool, BIO_POOL_SIZE, 0, 0);

	if (status) {
		pr_err("Couldn't allocate bio set");
		goto mem_err;
	}

	INIT_LIST_HEAD(&bd_list);

	lsbdd_sectors_cache = kmem_cache_create("sectors_cache", sizeof(struct sectors), 0, SLAB_HWCACHE_ALIGN, NULL);
	lsbdd_value_cache = kmem_cache_create("value_cache", sizeof(struct value_redir), 0, SLAB_HWCACHE_ALIGN, NULL);
	lsbdd_hash_cache = kmem_cache_create("hash_cache", sizeof(struct hash_el), 0, SLAB_HWCACHE_ALIGN, NULL);

	if (!(lsbdd_value_cache && lsbdd_sectors_cache && lsbdd_hash_cache)) {
		pr_err("Couldn't allocate cache\n");
		goto mem_err;
	}
	
	return 0;

mem_err:
	kfree(bdd_pool);
	pr_err("Memory allocation failed\n");
	return -ENOMEM;
}

static void __exit lsbdd_exit(void)
{
	u16 i = 0;
	struct bd_manager *entry, *tmp;

	if (!list_empty(&bd_list)) {
		while (get_list_element_by_index(i) != NULL)
			delete_bd(i + 1);
	}

	list_for_each_entry_safe(entry, tmp, &bd_list, list) {
		list_del(&entry->list);
		kfree(entry);
	}

	bioset_exit(bdd_pool);
	kfree(bdd_pool);
	unregister_blkdev(bdd_major, LSBDD_BLKDEV_NAME_PREFIX);

	pr_debug("BDR module exited\n");
}

static const struct kernel_param_ops lsbdd_delete_ops = {
	.set = lsbdd_delete_bd,
	.get = NULL,
};

static const struct kernel_param_ops lsbdd_get_bd_ops = {
	.set = NULL,
	.get = lsbdd_get_vbd_names,
};

static const struct kernel_param_ops lsbdd_redirect_ops = {
	.set = lsbdd_set_redirect_bd,
	.get = NULL,
};

static const struct kernel_param_ops lsbdd_ds_ops = {
	.set = lsbdd_set_data_struct,
	.get = lsbdd_get_data_structs,
};

MODULE_PARM_DESC(delete_bd, "Delete BD");
module_param_cb(delete_bd, &lsbdd_delete_ops, NULL, 0200);

MODULE_PARM_DESC(get_vbd_names, "Get list of disks and their redirect bd's");
module_param_cb(get_vbd_names, &lsbdd_get_bd_ops, NULL, 0644);

MODULE_PARM_DESC(set_redirect_bd, "Link local disk with redirect aim bd");
module_param_cb(set_redirect_bd, &lsbdd_redirect_ops, NULL, 0200);

MODULE_PARM_DESC(set_data_structure, "Set data structure to be used in mapping");
module_param_cb(set_data_structure, &lsbdd_ds_ops, NULL, 0644);

module_init(lsbdd_init);
module_exit(lsbdd_exit);
