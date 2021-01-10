#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/eventfd.h>
#include <linux/err.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/sort.h>
#include <linux/version.h>

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
#include <linux/buffer_head.h>
#include <linux/delay.h>
#include <linux/bio.h>
#include <linux/random.h>
#include <linux/buffer_head.h>
#include <linux/vmalloc.h>
#include <asm/segment.h>
#include <asm/uaccess.h>

#include "wrapfs.h"
#include "mmap_fault_handler.h"
#include "mmap_buffer_interface.h"
#include "mmap_buffer_hash_table.h"
#include "mmap_buffer_rbtree.h"
#include "dmap.h"

void write_bio_tagged_page(struct tagged_page *evicted_page)
{
  unsigned char __tmp[BIO_SPAGE_SIZE];
  struct bio *bio = (struct bio *)__tmp;
	int ret;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,9,0) && LINUX_VERSION_CODE < KERNEL_VERSION(4,10,0)
	bio_init(bio);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(4,14,0) && LINUX_VERSION_CODE < KERNEL_VERSION(4,15,0)
  bio_init(bio, bio->bi_inline_vecs, 1);
#endif

  bio->bi_pool = NULL;

  DMAP_BGON(evicted_page->pvd->bk.bdev == NULL);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,9,0) && LINUX_VERSION_CODE < KERNEL_VERSION(4,10,0)
	bio->bi_bdev = evicted_page->pvd->bk.bdev;
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(4,14,0) && LINUX_VERSION_CODE < KERNEL_VERSION(4,15,0)
	bio_set_dev(bio, evicted_page->pvd->bk.bdev);
#endif
  bio_set_op_attrs(bio, REQ_OP_WRITE, 0);
  bio->bi_iter.bi_sector = evicted_page->page->index << 3;

  ret = bio_add_page(bio, evicted_page->page, PAGE_SIZE, 0);
	DMAP_BGON(ret != PAGE_SIZE);

  submit_bio_wait(bio);
}

void write_file_tagged_page(struct tagged_page *evicted_page)
{
	struct file *lower_file = evicted_page->pvd->bk.filp;
	loff_t pos = evicted_page->page->index << PAGE_SHIFT;
	ssize_t retval;
#ifdef USE_DIRECT_IO
	struct inode *inode;
	ssize_t (*direct_IO)(struct kiocb *, struct iov_iter * iter);
	int overwrite = 0;
	struct kiocb iocb;
	struct bio_vec bvec;
	struct iov_iter iter;
#endif

	DMAP_BGON(evicted_page->pvd->is_valid == false);
	DMAP_BGON(lower_file == NULL);

#ifdef USE_DIRECT_IO
	inode = file_inode(lower_file);
	direct_IO = lower_file->f_inode->i_mapping->a_ops->direct_IO;

  iocb.ki_filp = lower_file;
  iocb.ki_pos = pos;
  iocb.ki_complete = NULL;
  iocb.private = &overwrite;
  iocb.ki_flags =  IOCB_DIRECT | IOCB_WRITE; // | IOCB_SYNC | IOCB_DSYNC,

  bvec.bv_page = evicted_page->page;
  bvec.bv_len = PAGE_SIZE;
  bvec.bv_offset = 0;

  iter.type = WRITE | ITER_BVEC;
  iter.iov_offset = 0;
  iter.count = PAGE_SIZE;
  iter.bvec = &bvec;
  iter.nr_segs = 1;

  get_file(lower_file); /* prevent lower_file from being released */
	inode_lock(inode);

	retval = direct_IO(&iocb, &iter);
  if(retval != PAGE_SIZE){
    printk(KERN_ERR "[%s:%s:%d] direct_IO(WRITE, ...) returns %zd\n", __FILE__, __func__, __LINE__, retval);
    DMAP_BGON(1);
  }

	inode_unlock(inode);
  fput(lower_file);
#else
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,9,0) && LINUX_VERSION_CODE < KERNEL_VERSION(4,10,0)
	retval = kernel_write(lower_file, (const char *)page_address(evicted_page->page), PAGE_SIZE, pos);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(4,14,0) && LINUX_VERSION_CODE < KERNEL_VERSION(4,15,0)
	retval = kernel_write(lower_file, page_address(evicted_page->page), PAGE_SIZE, &pos);
#endif
	if(retval != PAGE_SIZE){
    printk(KERN_ERR "[%s:%s:%d] kernel_write(...) returns %zd\n", __FILE__, __func__, __LINE__, retval);
		DMAP_BGON(1);
	}
#endif
}

