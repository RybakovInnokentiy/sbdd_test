#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/bio.h>
#include <linux/bvec.h>
#include <linux/init.h>
#include <linux/wait.h>
#include <linux/stat.h>
#include <linux/slab.h>
#include <linux/numa.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/blkdev.h>
#include <linux/string.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/vmalloc.h>
#include <linux/moduleparam.h>
#include <linux/spinlock_types.h>

#define SBDD_SECTOR_SHIFT       9
#define SBDD_SECTOR_SIZE        (1 << SBDD_SECTOR_SHIFT)
#define SBDD_MIB_SECTORS        (1 << (20 - SBDD_SECTOR_SHIFT))
#define SBDD_NAME               "sbdd"
#define TARGET_NAME_LEN         50

struct sbdd {
	wait_queue_head_t       exitwait;
	spinlock_t              datalock;
	atomic_t                deleting;
    atomic_t                redirecting;
	atomic_t                refs_cnt;
	sector_t                capacity;
	u8                      *data;
	struct gendisk          *gd;
    struct bdev_handle      *target_bdev_hdl; 
    struct block_device     *target_bdev;
};

static struct sbdd              __sbdd = { 0 };
static unsigned long            __sbdd_capacity_mib = 100;

static bool redirect_to_target;
static bool target_registered;

static char __target_bdev_name[TARGET_NAME_LEN];

static void target_bdev_end_bio(struct bio *bio) {
    struct bio *orig_bio = bio->bi_private;
    if (orig_bio) {
        orig_bio->bi_status = bio->bi_status;
        bio_endio(orig_bio);
    }

    bio_put(bio);
}

static int __always_inline sbdd_bio_prepare(struct bio **bio) {
    *bio = bio_split_to_limits(*bio);
	if (!(*bio))
		return EAGAIN;

    if (atomic_read(&__sbdd.deleting) || atomic_read(&__sbdd.redirecting)) {
        bio_io_error(*bio);
        return EAGAIN;
    }

    if (!atomic_inc_not_zero(&__sbdd.refs_cnt)) {
        bio_io_error(*bio);
        return EAGAIN;
    }
    
    return 0;
}

static sector_t sbdd_xfer(struct bio_vec* bvec, sector_t pos, int dir)
{
	void *buff = kmap_atomic(bvec->bv_page) + bvec->bv_offset;
	sector_t len = bvec->bv_len >> SBDD_SECTOR_SHIFT;
	size_t offset;
	size_t nbytes;

	if (pos + len > __sbdd.capacity)
		len = __sbdd.capacity - pos;

	offset = pos << SBDD_SECTOR_SHIFT;
	nbytes = len << SBDD_SECTOR_SHIFT;

	spin_lock(&__sbdd.datalock);

	if (dir)
		memcpy(__sbdd.data + offset, buff, nbytes);
	else
		memcpy(buff, __sbdd.data + offset, nbytes);

	spin_unlock(&__sbdd.datalock);

	pr_debug("pos=%6llu len=%4llu %s\n", pos, len, dir ? "written" : "read");

	kunmap_atomic(buff);
	return len;
}

static void sbdd_submit_bio_ram(struct bio *bio) 
{
	struct bvec_iter iter;
	struct bio_vec bvec;
	int dir;
	sector_t pos;

    int ret = sbdd_bio_prepare(&bio);
    if (ret) {
        pr_err("Can't prepare bio: %d. Retrying...\n", ret);
        return;
    }

    dir = bio_data_dir(bio);
    pos = bio->bi_iter.bi_sector;
    bio_for_each_segment(bvec, bio, iter)
        pos += sbdd_xfer(&bvec, pos, dir);

    bio_endio(bio);

    if (atomic_dec_and_test(&__sbdd.refs_cnt))
        wake_up(&__sbdd.exitwait);
}

static void sbdd_submit_bio_target(struct bio *bio) 
{
    int ret = sbdd_bio_prepare(&bio);
    if (ret) {
        pr_err("Can't prepare bio: %d. Retrying...\n", ret);
        return;
    }

    struct bio *bio_redirect = bio_alloc_clone(__sbdd.target_bdev, bio, GFP_NOIO, \
                                    &__sbdd.target_bdev->bd_disk->bio_split);
    bio_redirect->bi_end_io = target_bdev_end_bio;
    bio_redirect->bi_private = bio;
    submit_bio(bio_redirect);

    if (atomic_dec_and_test(&__sbdd.refs_cnt))
        wake_up(&__sbdd.exitwait);
}

static struct block_device_operations const __sbdd_bdev_ram_ops = {
	.owner = THIS_MODULE,
	.submit_bio = sbdd_submit_bio_ram,
};

static struct block_device_operations const __sbdd_bdev_target_ops = {
	.owner = THIS_MODULE,
	.submit_bio = sbdd_submit_bio_target,
};

static int sbdd_create(void)
{
	int ret = 0;

	pr_info("allocating data\n");
	__sbdd.capacity = (sector_t)__sbdd_capacity_mib * SBDD_MIB_SECTORS;
	__sbdd.data = vzalloc(__sbdd.capacity << SBDD_SECTOR_SHIFT);
	if (!__sbdd.data) {
		pr_err("unable to alloc data\n");
		return -ENOMEM;
	}

	spin_lock_init(&__sbdd.datalock);
	init_waitqueue_head(&__sbdd.exitwait);

	pr_info("allocating disk\n");
	__sbdd.gd = blk_alloc_disk(NUMA_NO_NODE);
	if (IS_ERR(__sbdd.gd)) {
		pr_err("blk_alloc_disk() failed\n");
		ret = PTR_ERR(__sbdd.gd);
		__sbdd.gd = NULL;
		return ret;
	}
    
	/* Configure queue */
	blk_queue_logical_block_size(__sbdd.gd->queue, SBDD_SECTOR_SIZE);
	blk_queue_physical_block_size(__sbdd.gd->queue, SBDD_SECTOR_SIZE);

	/* Configure gendisk */
	__sbdd.gd->private_data = &__sbdd;
	scnprintf(__sbdd.gd->disk_name, DISK_NAME_LEN, SBDD_NAME);
    if (target_registered && redirect_to_target) {
        set_capacity(__sbdd.gd, get_capacity(__sbdd.target_bdev->bd_disk));
	    __sbdd.gd->fops = &__sbdd_bdev_target_ops;
    } else {
        set_capacity(__sbdd.gd, __sbdd.capacity);
	    __sbdd.gd->fops = &__sbdd_bdev_ram_ops;
    }

	atomic_set(&__sbdd.refs_cnt, 1);

	/*
	Allocating gd does not make it available, add_disk() is required.
	After this call, gd methods can be called at any time. Should not be
	called before the driver is fully initialized and ready to process reqs.
	*/
	pr_info("adding disk\n");
	ret = add_disk(__sbdd.gd);
	if (ret)
		pr_err("add_disk() failed\n");

	return ret;
}

static void sbdd_delete(void)
{
	atomic_set(&__sbdd.deleting, 1);
	atomic_dec_if_positive(&__sbdd.refs_cnt);
	wait_event(__sbdd.exitwait, !atomic_read(&__sbdd.refs_cnt));

    if (__sbdd.target_bdev) {
        bdev_release(__sbdd.target_bdev_hdl);
    }
	/* gd will be removed only after the last reference put */
	if (__sbdd.gd) {
		pr_info("deleting disk\n");
		del_gendisk(__sbdd.gd);
		put_disk(__sbdd.gd);
	}

	if (__sbdd.data) {
		pr_info("freeing data\n");
		vfree(__sbdd.data);
	}
}

/*
Note __init is for the kernel to drop this function after
initialization complete making its memory available for other uses.
There is also __initdata note, same but used for variables.
*/
static int __init sbdd_init(void)
{
	int ret = 0;

	pr_info("starting initialization...\n");
	ret = sbdd_create();

	if (ret) {
		pr_err("initialization failed\n");
		sbdd_delete();
	} else {
		pr_info("initialization complete\n");
	}

	return ret;
}

/*
Note __exit is for the compiler to place this code in a special ELF section.
Sometimes such functions are simply discarded (e.g. when module is built
directly into the kernel). There is also __exitdata note.
*/
static void __exit sbdd_exit(void)
{
	pr_info("exiting...\n");
	sbdd_delete();
	pr_info("exiting complete\n");
}

static int target_redirect_set(const char *val, const struct kernel_param *kp) {
    int ret = 0;
    atomic_set(&__sbdd.redirecting, 1);

    if (strcmp(__target_bdev_name, "") == 0) {
        pr_err("Empty target name! Enter /sys/module/sbdd/target_bdev_name.\n");
        goto redir_out;
    }

    if(!target_registered) {
        __sbdd.target_bdev_hdl = bdev_open_by_path(__target_bdev_name, BLK_OPEN_READ | BLK_OPEN_WRITE, NULL, NULL);
        if (IS_ERR(__sbdd.target_bdev_hdl)) {
            ret = PTR_ERR(__sbdd.target_bdev_hdl);
            pr_err("Failed to open bdev with name %s. Aborting...\n", __target_bdev_name);
            goto redir_out;
        }
        __sbdd.target_bdev = __sbdd.target_bdev_hdl->bdev;
        target_registered = true;
        pr_info("Target %s registered.\n", __target_bdev_name);
    }
    
    ret = param_set_bool(val, kp);

    if (ret == 0)
        redirect_to_target = *(bool*) kp->arg;

    if (__sbdd.gd) {
        if (redirect_to_target) {
            set_capacity(__sbdd.gd, get_capacity(__sbdd.target_bdev->bd_disk));
	        __sbdd.gd->fops = &__sbdd_bdev_target_ops;
        } else {
            set_capacity(__sbdd.gd, __sbdd.capacity);
            __sbdd.gd->fops = &__sbdd_bdev_ram_ops;
        }
    }
    
redir_out:
    atomic_set(&__sbdd.redirecting, 0);
    return ret;
}

static int target_redirect_get(char *buf, const struct kernel_param *kp) {
    *(bool *) kp->arg = redirect_to_target;
    return param_get_bool(buf, kp);
}

static const struct kernel_param_ops target_redirect_ops = {
    .set = target_redirect_set,
    .get = target_redirect_get,
};

/* Called on module loading. Is mandatory. */
module_init(sbdd_init);

/* Called on module unloading. Unloading module is not allowed without it. */
module_exit(sbdd_exit);

/* Set desired capacity with insmod */
module_param_named(capacity_mib, __sbdd_capacity_mib, ulong, S_IRUGO);

module_param_cb(redirect, &target_redirect_ops, &redirect_to_target, 0664);
module_param_string(target_bdev_name, __target_bdev_name, TARGET_NAME_LEN, 0664);

/* Note for the kernel: a free license module. A warning will be outputted without it. */
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Simple Block Device Driver");
