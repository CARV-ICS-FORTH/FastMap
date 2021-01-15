#include <linux/init.h>
#include <linux/fs.h>
#include <linux/major.h>
#include <linux/blkdev.h>
#include <linux/backing-dev.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/capability.h>
#include <linux/uio.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/gfp.h>
#include <linux/compat.h>
#include <linux/vmalloc.h>
#include <linux/buffer_head.h>
#include <linux/delay.h>
#include <linux/bitmap.h>
#include <linux/mman.h>
#include <linux/mm.h>
#include <linux/version.h>
#include <linux/bitops.h>

#include <asm/uaccess.h>
#include <asm/tlbflush.h>

#include "dmap.h"
#include "dmap-ioctl.h"
#include "mmap_fault_handler.h"
#include "mmap_generational_fifo_buffer.h"
#include "shared_defines.h"

struct sys_rt_parameters_struct;

#include "di-mmap_parameters.h"

#define MODULE_NAME "dmap"

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,9,0) && LINUX_VERSION_CODE < KERNEL_VERSION(4,10,0)
#pragma message("Building for kernel 4.9.X!")
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(4,14,0) && LINUX_VERSION_CODE < KERNEL_VERSION(4,15,0)
#pragma message("Building for kernel 4.14.X!")
#endif

#ifdef __aarch64__
#pragma message("Building for ARM64")
#endif

#ifdef __x86_64__
#pragma message("Building for X86_64")
#endif

#define PAGES_SMALL 128
#define PAGES_1M 1024
#define PAGES_2M 2048
#define PAGES_16M 16384
#define PAGES_512M 131072
#define PAGES_1G 262144
#define PAGES_2G (262144 << 1)
#define PAGES_4G (262144 << 2)
#define PAGES_8G (262144 << 3)
#define PAGES_12G (PAGES_4G + PAGES_8G)
#define PAGES_16G (262144 << 4)
#define PAGES_20G (5242880)
#define PAGES_32G (262144 << 5)
#define PAGES_64G (262144 << 6)
#define PAGES_128G (262144 << 7)
#define PAGES_192G (PAGES_128G + PAGES_64G)

static int num_banks = 1;
module_param(num_banks, int, S_IRUGO);
MODULE_PARM_DESC(num_banks, "Number of internal banks in the buffer");

static long perma_mmap_buf_size = PAGES_16G;
module_param(perma_mmap_buf_size, long, S_IRUGO);
MODULE_PARM_DESC(perma_mmap_buf_size, "Number of pages allocated to buffering");

static struct class_attribute dimmap_class_attrs[256];
int perma_devs = 1; /* number of bare perma devices */

static struct class *raw_class;
static struct raw_device_data *raw_devices;
static DEFINE_MUTEX(raw_mutex);
static const struct file_operations raw_ctl_fops; /* forward declaration */

buf_ld_params_t *g_buf_ld_params = NULL;

static int DMAP_MAJOR = 0;
static int max_raw_minors = MAX_DI_MMAP_MINORS;

dimmap_buffer_t dimmap_buffer;

struct kmem_cache *pve_alloc_cache = NULL;

extern struct lpfifo_operations_struct *lpfifo_ops;
extern struct dfifo_operations_struct *dfifo_ops;

static void dimmap_class_release(struct class *cls) {}

extern void clear_mappings(struct tagged_page *p, struct evictor_tlb *etlb);
extern void write_bio_tagged_page(struct tagged_page *evicted_page);
extern void write_dirty_pages(struct pr_vma_data *pvd, void (*__page_cleanup)(struct tagged_page *));

extern void ino_cache_init(void);
extern int acquire_ksyms(void);

#ifdef USE_HUGEPAGES
struct page *hugepages[16];
spinlock_t hugepages_lock;
unsigned long hugepages_idx = 0;
#endif

buf_ld_params_t *to_dimmap_buf_ld_params(struct class *c)
{
	dimmap_buffer_t *dimmap_buf = to_dimmap_buf(c);
	return &dimmap_buf->ld_params;
}

buf_rt_params_t *to_dimmap_buf_rt_params(struct class *c)
{
	dimmap_buffer_t *dimmap_buf = to_dimmap_buf(c);
	if (dimmap_buf->buf_data == NULL)
	{
		return NULL;
	}
	return &dimmap_buf->buf_data->rt_params;
}

int update_dimmap_buf_ld_params(struct class *c)
{
	dimmap_buffer_t *dimmap_buf = to_dimmap_buf(c);
	int cached_buffer_state = dimmap_buf->sys_rt_params.buffer_state; /* Cache the current buffer state */

	if (cached_buffer_state != 0)
	{
		/* Unload / deallocate buffer */
		deallocate_runtime_buffer(dimmap_buf);
	}

	if (cached_buffer_state != 0)
	{
		/* Re-load / allocate buffer */
		allocate_and_init_runtime_buffer(dimmap_buf);
	}

	return 0;
}

int update_dimmap_buf_rt_params(struct class *c)
{
	dimmap_buffer_t *dimmap_buf = to_dimmap_buf(c);
	update_mmap_runtime_parameters(dimmap_buf->buf_data, NULL);
	return 0;
}

int deallocate_runtime_buffer(dimmap_buffer_t *dimmap_rt)
{
	cleanup_mmap_params();
	cleanup_mmap_buffer_data(buf_data);
	dimmap_rt->sys_rt_params.buffer_state = 0;
	buf_data = NULL;

	return 0;
}

int fast_fsync_device_pages(struct file *file, loff_t start, loff_t end, int datasync)
{
	DMAP_BGON(1);
	return 0;
}

/*
 * Open/close code for raw IO.
 *
 * We just rewrite the i_mapping for the /dev/raw/rawN file descriptor to
 * point at the blockdev's address_space and set the file handle to use
 * O_DIRECT.
 *
 * Set the device's soft blocksize to the minimum possible.  This gives the
 * finest possible alignment and has no adverse impact on performance.
 */
static int raw_open(struct inode *inode, struct file *filp)
{
	const int minor = iminor(inode);
	struct block_device *bdev;
	struct pr_vma_data *pvd;
	struct fastmap_info *fmap_info;
	int cpu, err;

	if (minor == 0)
	{ /* It is the control device */
		filp->f_op = &raw_ctl_fops;
		return 0;
	}

	mutex_lock(&raw_mutex);

	/*
	 * All we need to do on open is check that the device is bound.
	 */
	bdev = raw_devices[minor].binding;
	err = -ENODEV;
	if (!bdev)
		goto out;

	bdgrab(bdev);
	err = blkdev_get(bdev, filp->f_mode /* | FMODE_EXCL */, raw_open);
	if (err)
		goto out;

	err = set_blocksize(bdev, bdev_logical_block_size(bdev));
	if (err)
		goto out1;

	filp->f_flags |= O_DIRECT;
	filp->f_mapping = bdev->bd_inode->i_mapping;
	if (++raw_devices[minor].inuse == 1)
		file_inode(filp)->i_mapping = bdev->bd_inode->i_mapping;

	//filp->private_data = &raw_devices[minor];
	
	/* Allocate fastmap_info struct first */
	fmap_info = kzalloc(sizeof(struct fastmap_info), GFP_KERNEL);
	DMAP_BGON(fmap_info == NULL);
	/*
	 * Since kzalloc is used all fmap_info members are initialized
	 * to 0, therefore there is no need to set is_fastmap and pve
	 * to false and NULL respectively
	 */
	pvd = kzalloc(sizeof(struct pr_vma_data), GFP_KERNEL);
	DMAP_BGON(pvd == NULL);

	pvd->magic1 = PVD_MAGIC_1;
	pvd->type = D_BLKD;
	pvd->dev_id = raw_devices[minor].dev_id;
	pvd->lower_page_mkwrite = NULL;
	pvd->bk.bdev = raw_devices[minor].binding;
	pvd->meta.rdd = &raw_devices[minor];
	pvd->is_valid = true;

#ifdef USE_PERCPU_RADIXTREE
	for(cpu = 0; cpu < num_online_cpus(); cpu++){
		spin_lock_init(&pvd->radix_lock[cpu]);
		INIT_RADIX_TREE(&pvd->rdx[cpu], GFP_ATOMIC);
	}
#else
	spin_lock_init(&pvd->radix_lock);
	INIT_RADIX_TREE(&pvd->rdx, GFP_ATOMIC);
#endif

	spin_lock_init(&pvd->pvd_lock);

	for(cpu = 0; cpu < num_online_cpus(); cpu++){
		spin_lock_init(&(pvd->tree_lock[cpu]));
#ifdef USE_RADIX_TREE_FOR_DIRTY
		INIT_RADIX_TREE(&pvd->dirty_tree[cpu], GFP_ATOMIC);
#else
		pvd->dirty_tree[cpu] = RB_ROOT;
#endif
	}

	pvd->magic2 = PVD_MAGIC_2;

	fmap_info->pvd = pvd;

	filp->private_data = fmap_info;

	mutex_unlock(&raw_mutex);

	return 0;
out1:
	blkdev_put(bdev, filp->f_mode /* | FMODE_EXCL */);
out:
	mutex_unlock(&raw_mutex);
	return err;
}

static void blkdev_release_cb(struct tagged_page *p)
{
	move_page_d2c_fifo_buffer_t(buf_data->banks, p);
}

/*
 * When the final fd which refers to this character-special node is closed, we
 * make its ->mapping point back at its own i_data.
 */
static int raw_release(struct inode *inode, struct file *filp)
{
	const int minor = iminor(inode);
	struct block_device *bdev;
	struct pr_vma_data *pvd;
#ifdef USE_PERCPU_RADIXTREE
	int cpu;
#endif

	printk(KERN_ERR "[%s:%s:%d] Calling release\n", __FILE__, __func__, __LINE__);

	pvd = ((struct fastmap_info *)filp->private_data)->pvd;
  if(pvd != NULL){


    //if(atomic64_read(&pvd->vma_count) == 0){
      struct tagged_page *p;
      void **slot;
      struct radix_tree_iter iter;
      pgoff_t start = 0;
			uint64_t num_frees = 0;

			//printk(KERN_ERR "[%s:%s:%d] PVD is not NULL\n", __FILE__, __func__, __LINE__);
			//printk(KERN_ERR "[%s:%s:%d] vma_count = 0\n", __FILE__, __func__, __LINE__);

			write_dirty_pages(pvd, blkdev_release_cb);

      // we have to clear radix_tree
      while(true){
        p = NULL;

#ifdef USE_PERCPU_RADIXTREE
				for(cpu = 0; cpu < num_online_cpus(); cpu++){
        	rcu_read_lock();
        	radix_tree_for_each_slot(slot, &pvd->rdx[cpu], &iter, start){
          	p = radix_tree_deref_slot(slot);
          	radix_tree_iter_delete(&pvd->rdx[cpu], &iter, slot);
          	break;
        	}
        	rcu_read_unlock();

					if(p != NULL)
						break;
				}
#else
				rcu_read_lock();
				radix_tree_for_each_slot(slot, &pvd->rdx, &iter, start){
					p = radix_tree_deref_slot(slot);
					radix_tree_iter_delete(&pvd->rdx, &iter, slot);
					break;
				}
				rcu_read_unlock();
#endif

        if(p == NULL)
          break;

				while(trylock_tp(p, ULONG_MAX) != 0)
					wait_on_page_bit(p->page, PG_locked);

				DMAP_BGON(atomic_read(&p->in_use) == 0);

				if(atomic_read(&p->buffer_id) == BI_CLEAN){
        	spin_lock(&buf_data->banks->primary_fifo_data[p->page->index % NUM_QUEUES]->qlock);
        	lpfifo_ops->remove(buf_data->banks->primary_fifo_data[p->page->index % NUM_QUEUES], p);
        	spin_unlock(&buf_data->banks->primary_fifo_data[p->page->index % NUM_QUEUES]->qlock);
				}else if(atomic_read(&p->buffer_id) == BI_DIRTY){
					int id = (p->page->index >> 9) % EVICTOR_THREADS;
					dfifo_t *dirty_data = buf_data->banks->dirty_queue[id];
					printk(KERN_ERR "[%s:%s:%d] Why do we have dirty pages here??\n", __FILE__, __func__, __LINE__);
					spin_lock(&dirty_data->dlock);
					dfifo_ops->remove(dirty_data, p);
					spin_unlock(&dirty_data->dlock);
				}else
					DMAP_BGON(1);	

        free_page_lock(&(buf_data->banks->page_map), p); /* it requires the page locked */
				num_frees++;
      }

			//printk(KERN_ERR "[%s:%s:%d] num_frees = %llu\n", __FILE__, __func__, __LINE__, num_frees);
   // }else{
			// atomic64_read(&pvd->vma_count) > 0
	//		printk(KERN_ERR "[%s:%s:%d] vma_count is > 0\n", __FILE__, __func__, __LINE__);
	//	}
  }else{
		// PVD is null
		printk(KERN_ERR "[%s:%s:%d] PVD is NULL\n", __FILE__, __func__, __LINE__);
	}

	mutex_lock(&raw_mutex);
	bdev = raw_devices[minor].binding;
	if(--raw_devices[minor].inuse == 0)
		inode->i_mapping = &inode->i_data; /* Here  inode->i_mapping == bdev->bd_inode->i_mapping  */
	kfree(filp->private_data);
	filp->private_data = NULL;
	mutex_unlock(&raw_mutex);

	blkdev_put(bdev, filp->f_mode /* | FMODE_EXCL */);

	return 0;
}

/*
 * Forward ioctls to the underlying block device.
 */
static long raw_ioctl(struct file *filp, unsigned int command, unsigned long arg)
{
	int retval = 0;
	struct pr_vma_data *pvd = (struct pr_vma_data *)filp->private_data;
	struct raw_device_data *rdd = pvd->meta.rdd;
	struct block_device *bdev = pvd->bk.bdev;
#if 0
	struct page *pg;
	struct tagged_page *tagged_page = NULL;
#endif
#ifdef ENABLE_STATS
	struct device_meta *m;
#endif

	struct dmap_page_prio __user *__dpp_p;
	struct dmap_page_prio __dpp_d;

	struct dmap_page_stats __user *__dps_p;
	struct dmap_page_stats __dps_d;

#if 0
	struct dmap_prefetch __user *__dp_p;
	struct dmap_prefetch *__dp_d;

	struct dmap_dontneed __user *__dd_p;
	struct dmap_dontneed __dd_d;
#endif

	uint64_t i;
	sector_t capacity;
	struct fake_blk_stats __user *__tos;
	struct fake_blk_page_range __user *__fromr;
	struct fake_blk_page_range __tor;
	struct fake_blk_page_num __user *__top;
	struct fake_blk_page_num __fromp;
	struct fake_blk_page_bitmap __user *__frombi;
	struct fake_blk_page_bitmap *__tobi;
	struct fake_blk_pages_num __user *__frompgs;
	struct fake_blk_pages_num *__topgs;
	int tb;

	DMAP_BGON(rdd == NULL);
	DMAP_BGON(rdd->binding != pvd->bk.bdev);
	DMAP_BGON(rdd->meta == NULL);

	if (_IOC_TYPE(command) != DMAP_IOC_MAGIC)
	{
		if (bdev != NULL)
			return blkdev_ioctl(bdev, 0, command, arg);
		else
			return -ENOTTY; // is this possible?
	}

	if (_IOC_NR(command) >= DMAP_IOC_MAXNR) {
		return -ENOTTY;
	}

	switch (command)
	{
	case DMAP_SET_PAGE_PRIORITY:
	{
		__dpp_p = (struct dmap_page_prio __user *)arg;
		retval = copy_from_user((void *)&__dpp_d, __dpp_p, sizeof(struct dmap_page_prio));
		if (retval != 0)
			return -EFAULT;

		if (__dpp_d.pageno >= rdd->device_size_in_pages)
		{
			printk(KERN_ERR "ERROR: We are going to set priority %u to page %llu\n", __dpp_d.prio, __dpp_d.pageno);
			return -EFAULT;
		}

		if (__dpp_d.prio >= MAX_PRIORITIES)
			rdd->meta[__dpp_d.pageno].priority = 0;
		else
			rdd->meta[__dpp_d.pageno].priority = __dpp_d.prio; // TODO maybe do some checks here

		break;
	}
	case DMAP_CHANGE_PAGE_PRIORITY:
	{
		__dpp_p = (struct dmap_page_prio __user *)arg;
		retval = copy_from_user((void *)&__dpp_d, __dpp_p, sizeof(struct dmap_page_prio));
		if (retval != 0)
			return -EFAULT;

		rdd->meta[__dpp_d.pageno].priority = __dpp_d.prio; // TODO maybe do some checks here
		change_page_priority(buf_data, __dpp_d.pageno, pvd, __dpp_d.prio);
		break;
	}
	case DMAP_GETPAGE_STATS:
	{
		__dps_p = (struct dmap_page_stats __user *)arg;
		retval = copy_from_user((void *)&__dps_d, __dps_p, sizeof(struct dmap_page_stats));
		if (retval != 0)
			return -EFAULT;

#ifdef ENABLE_STATS
		m = &(rdd->meta[__dps_d.pageno]);
#define COPY_ATTR(x) __dps_d.x = m->x
		COPY_ATTR(priority);
		COPY_ATTR(num_page_faults);
		COPY_ATTR(major_page_faults);
		COPY_ATTR(minor_page_faults);
		COPY_ATTR(num_evict);
		COPY_ATTR(num_evict_clean);
		COPY_ATTR(num_evict_dirty);
		COPY_ATTR(num_syncs);
		COPY_ATTR(num_reads);
		COPY_ATTR(num_mkwrite);
		COPY_ATTR(num_mkwrite_failed);
		COPY_ATTR(num_hit_in_buffer);
		COPY_ATTR(num_hit_in_buffer_valid);
		COPY_ATTR(num_hit_in_buffer_invalid);
		COPY_ATTR(num_recovered);
#undef COPY_ATTR
#endif /* ENABLE_STATS */
		retval = copy_to_user(__dps_p, (void *)&__dps_d, sizeof(struct dmap_page_stats));
		if (retval != 0)
			return -EFAULT;

		break;
	}
	case DMAP_DONTNEED:
	{
		break;
	}
	case DMAP_PREFETCH:
	{
		break;
	}
	case FAKE_BLK_IOC_TEST_CAP:
		break; // in order to return zero
	case FAKE_BLK_IOC_RESET_STATS:
		//Init_Statistics(); // FIXME
		break;
	case FAKE_BLK_IOC_GET_STATS:
		__tos = (struct fake_blk_stats __user *)arg;
		//retval = copy_to_user(__tos, (void *)&fbd_stat, sizeof(struct fake_blk_stats)); // FIXME
		if (retval != 0)
			return -EFAULT;
		break;
	case FAKE_BLK_IOC_ZERO_FULL:
		bitmap_zero(rdd->bitmap.bitmap, rdd->bitmap.bitmap_size);
		break;
	case FAKE_BLK_IOC_FILL_FULL:
		bitmap_fill(rdd->bitmap.bitmap, rdd->bitmap.bitmap_size);
		break;
	case FAKE_BLK_IOC_ZERO_PAGE:
		__top = (struct fake_blk_page_num __user *)arg;
		retval = copy_from_user((void *)&__fromp, __top, sizeof(struct fake_blk_page_num));
		if (retval != 0)
			return -EFAULT;
		if (__fromp.num >= rdd->bitmap.bitmap_size)
			return -EINVAL;

		clear_bit(__fromp.num, rdd->bitmap.bitmap);
		break;
	case FAKE_BLK_IOC_FILL_PAGE:
		__top = (struct fake_blk_page_num __user *)arg;
		retval = copy_from_user((void *)&__fromp, __top, sizeof(struct fake_blk_page_num));
		if (retval != 0)
			return -EFAULT;
		if (__fromp.num >= rdd->bitmap.bitmap_size)
			return -EINVAL;

		set_bit(__fromp.num, rdd->bitmap.bitmap);
		break;
	case FAKE_BLK_IOC_TEST_PAGE:
		__top = (struct fake_blk_page_num __user *)arg;
		retval = copy_from_user((void *)&__fromp, __top, sizeof(struct fake_blk_page_num));
		if (retval != 0)
			return -EFAULT;
		if (__fromp.num >= rdd->bitmap.bitmap_size)
			return -EINVAL;

#ifdef __aarch64__
		retval = test_bit(__fromp.num, rdd->bitmap.bitmap);
#else
		retval = constant_test_bit(__fromp.num, rdd->bitmap.bitmap);
#endif
		break;
	case FAKE_BLK_IOC_ZERO_RANGE:
		__fromr = (struct fake_blk_page_range __user *)arg;
		retval = copy_from_user((void *)&__tor, __fromr, sizeof(struct fake_blk_page_range));
		if (retval != 0)
			return -EFAULT;
		if (__tor.offset >= rdd->bitmap.bitmap_size)
			return -EINVAL;
		if (__tor.offset + __tor.length > rdd->bitmap.bitmap_size)
			return -EINVAL;

		bitmap_clear(rdd->bitmap.bitmap, __tor.offset, __tor.length);
		break;
	case FAKE_BLK_IOC_FILL_RANGE:
		__fromr = (struct fake_blk_page_range __user *)arg;
		retval = copy_from_user((void *)&__tor, __fromr, sizeof(struct fake_blk_page_range));
		if (retval != 0)
			return -EFAULT;
		if (__tor.offset >= rdd->bitmap.bitmap_size)
			return -EINVAL;
		if (__tor.offset + __tor.length > rdd->bitmap.bitmap_size)
			return -EINVAL;

		bitmap_set(rdd->bitmap.bitmap, __tor.offset, __tor.length);
		break;
	case FAKE_BLK_IOC_GET_DEVPGNUM:
		__top = (struct fake_blk_page_num __user *)arg;
		capacity = get_capacity(bdev->bd_disk);
		__fromp.num = capacity >> 3;
		retval = copy_to_user(__top, (void *)&__fromp, sizeof(struct fake_blk_page_num));
		if (retval != 0)
			return -EFAULT;
		break;
	case FAKE_BLK_IOC_FLIP_COPY_BITMAP: // fixed size of 4088 enties
		__frombi = (struct fake_blk_page_bitmap __user *)arg;
		__tobi = (struct fake_blk_page_bitmap *)kmalloc(sizeof(struct fake_blk_page_bitmap), GFP_NOIO);

		retval = copy_from_user((void *)__tobi, __frombi, sizeof(struct fake_blk_page_bitmap));
		if (retval != 0)
		{
			kfree(__tobi);
			return -EFAULT;
		}

		if ((__tobi->offset + BYTES_TO_BITS(4088)) > rdd->bitmap.bitmap_size)
			return -EINVAL;

		for (i = 0; i < BYTES_TO_BITS(4088); i++)
		{
			change_bit(i, (unsigned long *)(&(__tobi->bpage[0])));
#ifdef __aarch64__
			tb = test_bit(i, (unsigned long *)(&(__tobi->bpage[0])));
#else
			tb = constant_test_bit(i, (unsigned long *)(&(__tobi->bpage[0])));
#endif
			if (tb == 1)
				set_bit(__tobi->offset + i, rdd->bitmap.bitmap);
			else if (tb == 0)
				clear_bit(__tobi->offset + i, rdd->bitmap.bitmap);
			else
				printk(KERN_ERR "ERROR! constant_test_bit returns [%d] i = %llu\n", tb, i);
		}

		kfree(__tobi);
		break;
	case FAKE_BLK_IOC_ZERO_PAGES:
		__frompgs = (struct fake_blk_pages_num *)arg;
		__topgs = (struct fake_blk_pages_num *)kmalloc(sizeof(struct fake_blk_pages_num), GFP_NOIO);

		retval = copy_from_user((void *)__topgs, __frompgs, sizeof(struct fake_blk_pages_num));
		if (retval != 0)
		{
			kfree(__topgs);
			return -EFAULT;
		}

		for (i = 0; i < __topgs->num; i++)
		{
			if (__topgs->blocks[i] > rdd->bitmap.bitmap_size)
			{
				kfree(__topgs);
				return -EINVAL;
			}
			clear_bit(__topgs->blocks[i], rdd->bitmap.bitmap);
		}

		kfree(__topgs);
		break;
	default:
		printk(KERN_ERR "%s:%s:%d Unknown ioctl(%u)!\n", __FILE__, __func__, __LINE__, command);
		return -EINVAL;
	}

	return retval;
}

static int bind_set(int number, u64 major, u64 minor, char *path)
{
	dev_t dev = MKDEV(major, minor);
	struct raw_device_data *rawdev;
	int err = 0;
	int ret;

	printk(KERN_ERR "%s:%s:%d number = %d\n", __FILE__, __func__, __LINE__, number);
	printk(KERN_ERR "%s:%s:%d path = %s, major = %llu, minor = %llu\n", __FILE__, __func__, __LINE__, path, major, minor);

	if (number <= 0 || number >= max_raw_minors)
		return -EINVAL;

	if (MAJOR(dev) != major || MINOR(dev) != minor)
		return -EINVAL;

	rawdev = &raw_devices[number];

	printk(KERN_ERR "%s:%s:%d rawdev address = %p\n", __FILE__, __func__, __LINE__, rawdev);

	/*
	 * This is like making block devices, so demand the
	 * same capability
	 */
	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	/*
	 * For now, we don't need to check that the underlying
	 * block device is present or not: we can do that when
	 * the raw device is opened.  Just check that the
	 * major/minor numbers make sense.
	 */

	if (MAJOR(dev) == 0 && dev != 0)
		return -EINVAL;

	mutex_lock(&raw_mutex);
	if (rawdev->inuse)
	{
		mutex_unlock(&raw_mutex);
		return -EBUSY;
	}

	if (rawdev->binding)
	{
		bdput(rawdev->binding);
		module_put(THIS_MODULE);
	}

	if (!dev)
	{
		/* unbind */
		if (rawdev->meta != NULL)
			vfree(rawdev->meta);
		blkdev_put(rawdev->binding, FMODE_READ | FMODE_WRITE | FMODE_EXCL);
		rawdev->binding = NULL;
		device_destroy(raw_class, MKDEV(DMAP_MAJOR, number));
	}
	else
	{
		rawdev->binding = bdget(dev);
		rawdev->dev_id = MKDEV(major, minor);
		bdgrab(rawdev->binding);
		ret = blkdev_get(rawdev->binding, FMODE_READ | FMODE_WRITE | FMODE_EXCL, rawdev);
		printk(KERN_ERR "%s:%s:%d binding = %p ret = %d\n", __FILE__, __func__, __LINE__, rawdev->binding, ret);
		if (rawdev->binding == NULL)
		{
			err = -ENOMEM;
		}
		else
		{
			sector_t num_sect;
			dev_t raw = MKDEV(DMAP_MAJOR, number);

			strcpy(rawdev->path, path);
			printk(KERN_ERR "%s:%s:%d rawdev->binding->bd_disk = %p\n", __FILE__, __func__, __LINE__, rawdev->binding->bd_disk);

			// allocate page metadata
			num_sect = get_capacity(rawdev->binding->bd_disk);
			rawdev->device_size_in_pages = num_sect >> 3;
			rawdev->meta = (struct device_meta *)vmalloc((num_sect >> 3) * sizeof(struct device_meta));
			if (rawdev->meta == NULL)
				printk(KERN_ERR "%s:%s:%d Cannot allocate memory for metadata\n", __FILE__, __func__, __LINE__);

			memset(rawdev->meta, 0, (num_sect >> 3) * sizeof(struct device_meta));
			printk(KERN_ERR "%s:%s:%d Device has %llu sectors, %llu pages and %llu bytes\n", __FILE__, __func__, __LINE__,
				   (unsigned long long)num_sect, (unsigned long long)num_sect >> 3, (unsigned long long)num_sect * 512);

			// ***************** FAKE BLK ******************************
			memset(&(rawdev->bitmap), 0, sizeof(struct fbd_bitmap));
			if (rawdev->bitmap.bitmap == NULL)
			{
				sector_t capacity = get_capacity(rawdev->binding->bd_disk); //given in sectors
				rawdev->bitmap.bitmap_size = (capacity >> 3);				// how many pages in total
				rawdev->bitmap.bitmap = vmalloc(sizeof(unsigned long) * BITS_TO_LONGS(rawdev->bitmap.bitmap_size));
				bitmap_zero(rawdev->bitmap.bitmap, rawdev->bitmap.bitmap_size);
				printk(KERN_ERR "Bitmap size = %llu\n", rawdev->bitmap.bitmap_size);
			}
			// ***************** FAKE BLK ******************************

			__module_get(THIS_MODULE);
			device_destroy(raw_class, raw);
			device_create(raw_class, NULL, raw, NULL, "dmap%d", number);
		}
	}

	mutex_unlock(&raw_mutex);

	return err;
}

static int bind_get(int number, dev_t *dev)
{
	struct raw_device_data *rawdev;
	struct block_device *bdev;

	if (number <= 0 || number >= max_raw_minors)
		return -EINVAL;

	rawdev = &raw_devices[number];

	mutex_lock(&raw_mutex);
	bdev = rawdev->binding;
	*dev = bdev ? bdev->bd_dev : 0;
	mutex_unlock(&raw_mutex);
	return 0;
}

/*
 * Deal with ioctls against the raw-device control interface, to bind
 * and unbind other raw devices.
 */
static long raw_ctl_ioctl(struct file *filp, unsigned int command, unsigned long arg)
{
	struct dmap_config_request rq;
	dev_t dev;
	int err = -1;

	printk(KERN_ERR "raw_ctl_ioctl() called!\n");

	switch (command)
	{
	case DMAP_SETBIND:
		printk(KERN_ERR "DMAP_SETBIND\n");
		if (copy_from_user(&rq, (void __user *)arg, sizeof(rq)))
			return -EFAULT;

		return bind_set(rq.raw_minor, rq.block_major, rq.block_minor, rq.dev_path);
	case DMAP_GETBIND:
		printk(KERN_ERR "DMAP_GETBIND\n");
		if (copy_from_user(&rq, (void __user *)arg, sizeof(rq)))
			return -EFAULT;

		err = bind_get(rq.raw_minor, &dev);
		if (err)
			return err;

		rq.block_major = MAJOR(dev);
		rq.block_minor = MINOR(dev);

		if (copy_to_user((void __user *)arg, &rq, sizeof(rq)))
			return -EFAULT;

		return 0;
	default:
		printk(KERN_ERR "raw_ctl_ioctl(%u) -- unknown ioctl number!\n", command);
		DMAP_BGON(1);
	}

	return -EINVAL;
}

int dmap_pte_entry(pte_t *pte, unsigned long addr, unsigned long next, struct mm_walk *walk)
{
	if (!pte_none(*pte))
	{
		//printk(KERN_ERR "FOUND DIRTY PAGE MSYNC PAGETABLE!\n");
		// XXX do we need both?
		*pte = pte_mkclean(*pte);
		*pte = pte_wrprotect(*pte);
	}
	return 0;
}

#if 0
static void __dmap_page_cleanup(struct tagged_page *p)
{
	//ClearTaggedPageDirty(p);
	//atomic_set(&p->is_dirty, 0);
	move_page_d2c_fifo_buffer_t(buf_data->banks, p);
}
#endif

static void blkdev_fsync_cb(struct tagged_page *p)
{
	clear_mappings(p, NULL); /* also invalidates TLB */
	move_page_d2c_fifo_buffer_t(buf_data->banks, p);
}

int dmap_fsync(struct file *file, loff_t start, loff_t end, int datasync)
{
#if 1
	struct mm_struct *mm = current->mm;
	struct pr_vma_data *pvd = ((struct fastmap_info *)file->private_data)->pvd;
	
	printk(KERN_ERR "Calling msync() .....\n");
	down_write(&mm->mmap_sem);

	write_dirty_pages(pvd, blkdev_fsync_cb);

	up_write(&mm->mmap_sem);
	printk(KERN_ERR "Calling msync() ..... DONE!\n");
#endif
#if 0
	int ret = 0;
#ifdef LOCK_MSYNC
	struct mm_struct *mm;
	struct vm_area_struct *tmp;	
	struct pr_vma_data *pvd = (struct pr_vma_data *)file->private_data;
	struct mm_walk dmap_walk = {
		.pte_entry = dmap_pte_entry,
	};

	mm = current->mm;

	down_write(&mm->mmap_sem);
#endif


	//ret = fast_fsync_device_pages(file, start, end, datasync);
	write_dirty_pages(pvd, __dmap_page_cleanup);

#ifdef LOCK_MSYNC
	for (tmp = mm->mmap; tmp != NULL; tmp = tmp->vm_next)
	{
		if (tmp->vm_ops && tmp->vm_ops->name != NULL)
		{
			if (strcmp(tmp->vm_ops->name(tmp), "[dmap]") == 0)
			{
				dmap_walk.mm = tmp->vm_mm;
				walk_page_vma(tmp, &dmap_walk);
				flush_tlb_range(tmp, tmp->vm_start, tmp->vm_end);
			}
		}
	}

	up_write(&mm->mmap_sem);
#endif


	return ret;
#endif
	return 0;
}

extern int dmap_mmap_blockdev(struct file *filp, struct vm_area_struct *vma);

static const struct file_operations raw_fops = {
	.fsync = dmap_fsync,
	.open = raw_open,
	.release = raw_release,
	.unlocked_ioctl = raw_ioctl,
	.llseek = default_llseek,
	.mmap = dmap_mmap_blockdev,
	.owner = THIS_MODULE,
};

static const struct file_operations raw_ctl_fops = {
	.unlocked_ioctl = raw_ctl_ioctl,
	.open = raw_open,
	.owner = THIS_MODULE,
};

static struct cdev raw_cdev;

static char *raw_devnode(struct device *dev, umode_t *mode)
{
	return kasprintf(GFP_KERNEL, "dmap/%s", dev_name(dev));
}

int reset_device_parameters(void)
{
	int err;
	err = reset_mmap_params(&dimmap_buffer.ld_params);
	if (err)
	{
		printk(KERN_ALERT "Failed to reset mmap parameters\n");
		return err;
	}
	reset_mmap_state();

	return err;
}

int allocate_and_init_runtime_buffer(dimmap_buffer_t *dimmap_rt)
{
	mmap_buffer_t *new_buf_data, *dummy_buf_data = NULL;
	int ret = 0, i;

	/* Allocate a buffer for the di-mmap module */
	new_buf_data = init_mmap_buffer_data(dummy_buf_data, &dimmap_rt->ld_params, 0);
	if (new_buf_data == NULL)
	{
		printk(KERN_ERR "DI-MMAP: Failed to initialize the buffer data, aborting\n");
		ret = -ENOMEM;
		return ret;
	}

	dimmap_rt->buf_data = new_buf_data;
	buf_data = new_buf_data;

	printk(KERN_ALERT "DI-MMAP: Initializing the buffer data...\n");

	/* Clear out the data structures */
	memset(raw_devices, 0, MAX_DI_MMAP_MINORS * sizeof(struct raw_device_data));

	for (i = 0; i < MAX_DI_MMAP_MINORS; i++)
		raw_devices[i].buffer = dimmap_rt;

	/* Once all of the data structures are allocated, initialize them */
	ret = reset_device_parameters();
	if (ret != 0)
		goto error_init_mmap_buffer_data;

	/* Once the buffer is fully allocated, change the buffer state variable and then start the writeback daemon */
	dimmap_rt->sys_rt_params.buffer_state = 1;

	return 0;

error_init_mmap_buffer_data:
	/* Do any cleanup that the buffer requires */
	cleanup_mmap_buffer_data(buf_data);
	dimmap_rt->sys_rt_params.buffer_state = 0;
	buf_data = NULL;

	return ret;
}

extern int init_wrapfs_fs(void);
extern void exit_wrapfs_fs(void);

static void pve_init_once(void *foo)
{
	struct pr_vma_entry *pve = foo;
	int cpu;

	for(cpu = 0; cpu < num_online_cpus(); cpu++){
		INIT_LIST_HEAD(&pve->pcpu_mapped[cpu]);
		spin_lock_init(&pve->pcpu_mapped_lock[cpu]);
	}

	kref_init(&pve->refcount);
	pve->vm_start = 0;
	pve->vm_end = 0;
}

#ifdef USE_PROC_SYS_VM_COUNTERS 

atomic_t mm_lf_stat_pgfaults;
atomic_t mm_lf_stat_pgfaults_minor;
atomic_t mm_lf_stat_pgfaults_major;
atomic_t mm_lf_stat_mkwrites;
atomic_t mm_lf_stat_map_pages;
atomic_t mm_lf_stat_evicted_pages;
atomic_t mm_lf_stat_mapped_readahead_pgs;
atomic_t mm_lf_stat_failed_mapped_readahead_pgs;

atomic_t mm_lf_stat_fail_maps;
atomic_t mm_lf_stat_succ_maps;
atomic_t mm_lf_stat_call_maps;

#ifdef USE_PAGE_FAULT_FASTPATH

atomic_t mm_lf_pgfault_slow;
atomic_t mm_lf_pgfault_fast;
atomic_t mm_lf_pgfault_fast_failed_pte;
atomic_t mm_lf_pgfault_fast_failed_cas;

#endif

static struct ctl_table lf_stats_table[] = {
	{
		.procname = "page_faults",
		.data = &mm_lf_stat_pgfaults.counter,
		.maxlen = sizeof(mm_lf_stat_pgfaults),
		.mode   = 0444,
		.proc_handler = proc_dointvec,
	},
	{
		.procname = "page_faults_minor",
		.data = &mm_lf_stat_pgfaults_minor.counter,
		.maxlen = sizeof(mm_lf_stat_pgfaults_minor),
		.mode   = 0444,
		.proc_handler = proc_dointvec,
	},
	{
		.procname = "page_faults_major",
		.data = &mm_lf_stat_pgfaults_major.counter,
		.maxlen = sizeof(mm_lf_stat_pgfaults_major),
		.mode   = 0444,
		.proc_handler = proc_dointvec,
	},
	{
		.procname = "mkwrites",
		.data = &mm_lf_stat_mkwrites.counter,
		.maxlen = sizeof(mm_lf_stat_mkwrites),
		.mode   = 0444,
		.proc_handler = proc_dointvec,
	},
	{
		.procname = "map_pages",
		.data = &mm_lf_stat_map_pages.counter,
		.maxlen = sizeof(mm_lf_stat_map_pages),
		.mode   = 0444,
		.proc_handler = proc_dointvec,
	},
	{
		.procname = "evicted_pages",
		.data = &mm_lf_stat_evicted_pages.counter,
		.maxlen = sizeof(mm_lf_stat_evicted_pages),
		.mode   = 0444,
		.proc_handler = proc_dointvec,
	},
	{
		.procname = "mapped_rhead",
		.data = &mm_lf_stat_mapped_readahead_pgs.counter,
		.maxlen = sizeof(mm_lf_stat_mapped_readahead_pgs),
		.mode   = 0444,
		.proc_handler = proc_dointvec,
	},
	{
		.procname = "failed_mapped_rhead",
		.data = &mm_lf_stat_failed_mapped_readahead_pgs.counter,
		.maxlen = sizeof(mm_lf_stat_failed_mapped_readahead_pgs),
		.mode   = 0444,
		.proc_handler = proc_dointvec,
	},
	{
		.procname = "fail_maps",
		.data = &mm_lf_stat_fail_maps.counter,
		.maxlen = sizeof(mm_lf_stat_fail_maps),
		.mode   = 0444,
		.proc_handler = proc_dointvec,
	},
	{
		.procname = "succ_maps",
		.data = &mm_lf_stat_succ_maps.counter,
		.maxlen = sizeof(mm_lf_stat_succ_maps),
		.mode   = 0444,
		.proc_handler = proc_dointvec,
	},
	{
		.procname = "call_maps",
		.data = &mm_lf_stat_call_maps.counter,
		.maxlen = sizeof(mm_lf_stat_call_maps),
		.mode   = 0444,
		.proc_handler = proc_dointvec,
	},
#ifdef USE_PAGE_FAULT_FASTPATH
	{
		.procname = "pgfault_slow",
		.data = &mm_lf_pgfault_slow.counter,
		.maxlen = sizeof(mm_lf_pgfault_slow),
		.mode   = 0444,
		.proc_handler = proc_dointvec,
	},
	{
		.procname = "pgfault_fast",
		.data = &mm_lf_pgfault_fast.counter,
		.maxlen = sizeof(mm_lf_pgfault_fast),
		.mode   = 0444,
		.proc_handler = proc_dointvec,
	},
	{
		.procname = "pgfault_fast_failed_pte",
		.data = &mm_lf_pgfault_fast_failed_pte.counter,
		.maxlen = sizeof(mm_lf_pgfault_fast_failed_pte),
		.mode   = 0444,
		.proc_handler = proc_dointvec,
	},
	{
		.procname = "pgfault_fast_failed_cas",
		.data = &mm_lf_pgfault_fast_failed_cas.counter,
		.maxlen = sizeof(mm_lf_pgfault_fast_failed_cas),
		.mode   = 0444,
		.proc_handler = proc_dointvec,
	},
#endif
  { }
};

static struct ctl_path lf_stats_path[] = {
  { .procname = "vm" },
  { .procname = "lf_stats" },
  { }
};

static struct ctl_table_header *lf_stats_table_header;
#endif

static int __init raw_init(void)
{
	dev_t dev = MKDEV(DMAP_MAJOR, 1);
	int ret = 0, i;

	//DMAP_BGON(BIO_SPAGE_SIZE != (sizeof(struct bio) + sizeof(struct bio_vec)));

#ifdef USE_PROC_SYS_VM_COUNTERS
	atomic_set(&mm_lf_stat_pgfaults, 0);
	atomic_set(&mm_lf_stat_pgfaults_minor, 0);
	atomic_set(&mm_lf_stat_pgfaults_major, 0);
	atomic_set(&mm_lf_stat_mkwrites, 0);
	atomic_set(&mm_lf_stat_map_pages, 0);
	atomic_set(&mm_lf_stat_evicted_pages, 0);
	atomic_set(&mm_lf_stat_mapped_readahead_pgs, 0);
	atomic_set(&mm_lf_stat_failed_mapped_readahead_pgs, 0);
	atomic_set(&mm_lf_stat_fail_maps, 0);
	atomic_set(&mm_lf_stat_succ_maps, 0);
	atomic_set(&mm_lf_stat_call_maps, 0);

#ifdef USE_PAGE_FAULT_FASTPATH
	atomic_set(&mm_lf_pgfault_slow, 0);
	atomic_set(&mm_lf_pgfault_fast, 0);
	atomic_set(&mm_lf_pgfault_fast_failed_pte, 0);
	atomic_set(&mm_lf_pgfault_fast_failed_cas, 0);
#endif

	lf_stats_table_header = register_sysctl_paths(lf_stats_path, lf_stats_table);
	if(lf_stats_table_header == NULL)
		return -ENOMEM;
#endif

	ret = init_wrapfs_fs();
	if(ret){
		printk(KERN_ERR "Failed to initialize file system...\n");
		goto error_fs;
	}

	ino_cache_init();

	printk(KERN_ERR "sizeof(struct tagged_page) = %lu\n", sizeof(struct tagged_page));
	printk(KERN_ERR "sizeof(__u64) = %lu\n", sizeof(__u64));
	printk(KERN_ERR "sizeof(unsigned long) = %lu\n", sizeof(unsigned long));
	printk(KERN_ERR "sizeof(unsigned long long) = %lu\n", sizeof(unsigned long long));
	printk(KERN_ERR "num_online_cpus() = %d\n", num_online_cpus());

#ifdef USE_HUGEPAGES
	spin_lock_init(&hugepages_lock);
	for(i = 0; i < 16; i++){
		hugepages[i] = alloc_pages(GFP_NOIO, HPAGE_PMD_ORDER);
		if(hugepages[i] == NULL){
			printk(KERN_ERR "Cannot allocate huge page!\n");
			return -1;
		}
	}
#endif

	memset(&dimmap_buffer, 0, sizeof(dimmap_buffer_t));

	/* Try to find the unexported kernel symbols which we need */
	if(acquire_ksyms() == -1){
		printk(KERN_ERR "Could not translate all necessary kernel symbols\n");
		return -EINVAL;
	}

	/* Loadtime buffer parameters */
	init_ld_params(&dimmap_buffer.ld_params);
	dimmap_buffer.ld_params.mmap_buf_size = perma_mmap_buf_size >> 0;
	dimmap_buffer.ld_params.superpage_alloc_order = 0;

	/* System runtime parameters */
	init_sys_rt_params(&dimmap_buffer.sys_rt_params);

	if (max_raw_minors < 1 || max_raw_minors > 65536)
	{
		printk(KERN_WARNING "raw: invalid max_raw_minors (must be between 1 and 65536), using %d\n", MAX_RAW_MINORS);
		max_raw_minors = MAX_RAW_MINORS;
	}

	raw_devices = vzalloc(sizeof(struct raw_device_data) * max_raw_minors);
	if (!raw_devices)
	{
		printk(KERN_ERR "Not enough memory for raw device structures\n");
		ret = -ENOMEM;
		goto error;
	}

	ret = alloc_chrdev_region(&dev, 0, max_raw_minors, "dmap");
	if (ret)
		goto error;

	DMAP_MAJOR = MAJOR(dev);
	printk(KERN_ERR "dmap gets major number %d\n", DMAP_MAJOR);

	cdev_init(&raw_cdev, &raw_fops);
	ret = cdev_add(&raw_cdev, dev, max_raw_minors);
	if (ret)
		goto error_region;

	dimmap_buffer.dimmap_class.name = MODULE_NAME;
	dimmap_buffer.dimmap_class.owner = THIS_MODULE;

	/* Setup all of the loadtime and runtime class attributes */
	memset(&dimmap_class_attrs, 0, sizeof(struct class_attribute) * 256);

	i = 0;
	i = copy_sys_rt_param_class_attrs(dimmap_class_attrs, i);
	i = copy_ld_param_class_attrs(dimmap_class_attrs, i);
	i = copy_rt_param_class_attrs(dimmap_class_attrs, i);

	dimmap_buffer.dimmap_class.class_release = dimmap_class_release;
	ret = class_register(&dimmap_buffer.dimmap_class);
	if (ret)
	{
		printk(KERN_ERR "Error creating %s class.\n", MODULE_NAME);
		cdev_del(&raw_cdev);
		goto error_region;
	}

	dimmap_buffer.dimmap_class.devnode = raw_devnode;

	for (i = 0; i < 255 && dimmap_class_attrs[i].attr.name != NULL; ++i)
	{
		ret = class_create_file(&dimmap_buffer.dimmap_class, &dimmap_class_attrs[i]);
		if (ret)
			printk(KERN_ERR "Error creating %s class file.\n", dimmap_class_attrs[i].attr.name);
	}

	raw_class = &dimmap_buffer.dimmap_class;
	device_create(raw_class, NULL, MKDEV(DMAP_MAJOR, 0), NULL, "dmapctl");

	buf_data = NULL;
	
	pve_alloc_cache = kmem_cache_create("dmap_pve_cache", sizeof(struct pr_vma_entry), 0, (SLAB_RECLAIM_ACCOUNT|SLAB_MEM_SPREAD|SLAB_ACCOUNT), pve_init_once);
	DMAP_BGON(!pve_alloc_cache);

	return 0;

error_region:
	unregister_chrdev_region(dev, max_raw_minors);
error:
	vfree(raw_devices);
error_fs:
	return ret;
}

static void __exit raw_exit(void)
{
#ifdef USE_PROC_SYS_VM_COUNTERS
	unregister_sysctl_table(lf_stats_table_header);
#endif

	if (dimmap_buffer.sys_rt_params.buffer_state != 0)
		deallocate_runtime_buffer(&dimmap_buffer); /* Unload / deallocate buffer */

	device_destroy(raw_class, MKDEV(DMAP_MAJOR, 0));

	printk(KERN_ALERT "Destroy the class\n");
	class_unregister(&dimmap_buffer.dimmap_class);

	cdev_del(&raw_cdev);
	unregister_chrdev_region(MKDEV(DMAP_MAJOR, 0), max_raw_minors);

	exit_wrapfs_fs();

	kmem_cache_destroy(pve_alloc_cache);
}

module_init(raw_init);
module_exit(raw_exit);

MODULE_AUTHOR("Anastasios Papagiannis <apapag@ics.forth.gr>");
MODULE_LICENSE("LGPL-2.1");
