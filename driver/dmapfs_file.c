#include <asm/tlbflush.h>

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
#include <linux/bio.h>
#include <linux/random.h>
#include <linux/buffer_head.h>
#include <linux/hashtable.h>
#include <linux/version.h>
#include <asm/segment.h>
#include <asm/uaccess.h>

#include "wrapfs.h"
#include "dmap.h"
#include "dmap-ioctl.h"
#include "mmap_buffer_rbtree.h"
#include "shared_defines.h"

atomic64_t ms_in_open;
atomic64_t ms_in_fsync;
atomic64_t ms_in_get_page;

/* this keeps ino -> pvd mappings only for files
 * not block devices */
spinlock_t ino_pvd_lock;
struct radix_tree_root ino_pvd_cache;

extern struct lpfifo_operations_struct *lpfifo_ops;
extern struct dfifo_operations_struct *dfifo_ops;

extern banked_buffer_t *buf_data;

extern void remove_mappings(struct tagged_page *p, struct evictor_tlb *etlb);
extern void clear_mappings(struct tagged_page *p, struct evictor_tlb *etlb);
extern void write_dirty_pages(struct pr_vma_data *pvd, void (*__page_cleanup)(struct tagged_page *));
extern void write_file_tagged_page(struct tagged_page *evicted_page);
extern void move_page_d2c_fifo_buffer_t(fifo_buffer_t *buf, struct tagged_page *tagged_page);
extern unsigned int try_purge_pages_fast(fifo_buffer_t *buf, unsigned int N, int qid);
extern int fmap_radix_insert(struct pr_vma_data *pvd, unsigned long offset, struct tagged_page *p);
extern void dmap_do_page_io(struct vm_area_struct *vma, struct pr_vma_data *pvd, struct tagged_page **tagged_page, unsigned long *page_byte_offset, int num_pages);
extern void dmap_do_tagged_page_writable(struct vm_area_struct *vma, struct pr_vma_data *pvd, struct tagged_page *tagged_page);
extern struct tagged_page *fmap_radix_lookup(struct pr_vma_data *pvd, unsigned long offset);

void ino_cache_init(void)
{
	spin_lock_init(&ino_pvd_lock);
	INIT_RADIX_TREE(&ino_pvd_cache, GFP_ATOMIC);
}

static void __clear_pvd(struct pr_vma_data *pvd)
{
	struct tagged_page *p;
	void **slot;
	struct radix_tree_iter iter;
	pgoff_t start = 0;
#ifdef USE_PERCPU_RADIXTREE
	int cpu;	
#endif

  while(true){
  	p = NULL;

#ifdef USE_PERCPU_RADIXTREE
    for(cpu = 0; cpu < num_online_cpus(); cpu++){
        rcu_read_lock();
        radix_tree_for_each_slot(slot, &pvd->rdx[cpu], &iter, start){
          p = radix_tree_deref_slot(slot);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,14,0) && LINUX_VERSION_CODE < KERNEL_VERSION(4,15,0)
          radix_tree_iter_delete(&pvd->rdx[cpu], &iter, slot);
#endif
          break;
        }
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,9,0) && LINUX_VERSION_CODE < KERNEL_VERSION(4,10,0)
				if(p != NULL)
					radix_tree_delete(&pvd->rdx[cpu], p->page->index);
#endif
        rcu_read_unlock();

        if(p != NULL)
          break;
    }
#else
    rcu_read_lock();
    radix_tree_for_each_slot(slot, &pvd->rdx, &iter, start){
      p = radix_tree_deref_slot(slot);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,14,0) && LINUX_VERSION_CODE < KERNEL_VERSION(4,15,0)
      radix_tree_iter_delete(&pvd->rdx, &iter, slot);
#endif
      break;
    }
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,9,0) && LINUX_VERSION_CODE < KERNEL_VERSION(4,10,0)
		if(p != NULL)
			radix_tree_delete(&pvd->rdx, p->page->index);
#endif
    rcu_read_unlock();
#endif

    if(p == NULL)
      break;

		while(trylock_tp(p, ULONG_MAX) != 0)
			wait_on_page_bit(p->page, PG_locked);
		DMAP_BGON(atomic_read(&p->in_use) == 0);

    spin_lock(&buf_data->banks->primary_fifo_data[p->page->index % NUM_QUEUES]->qlock);
    lpfifo_ops->remove(buf_data->banks->primary_fifo_data[p->page->index % NUM_QUEUES], p);
  	spin_unlock(&buf_data->banks->primary_fifo_data[p->page->index % NUM_QUEUES]->qlock);

    free_page_lock(&(buf_data->banks->page_map), p); /* it requires the page locked */
  }
}

void ino_cache_clear(void)
{
	struct pr_vma_data *pvd;
	void **slot;
	struct radix_tree_iter iter;
	pgoff_t start = 0;
	
	while(true){
		pvd = NULL;

  	rcu_read_lock();
  	radix_tree_for_each_slot(slot, &ino_pvd_cache, &iter, start){
  		pvd = radix_tree_deref_slot(slot);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,14,0) && LINUX_VERSION_CODE < KERNEL_VERSION(4,15,0)
    	radix_tree_iter_delete(&ino_pvd_cache, &iter, slot);
#endif
    	break;
  	}
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,9,0) && LINUX_VERSION_CODE < KERNEL_VERSION(4,10,0)
		if(pvd != NULL)
			radix_tree_delete(&ino_pvd_cache, (unsigned long)pvd);
#endif
  	rcu_read_unlock();	

		if(pvd == NULL)
    	break;

		__clear_pvd(pvd);

		if(pvd->bk.filp != NULL)
			fput(pvd->bk.filp);
#if 0
    if(pvd->meta.f.m != NULL)
    	vfree(pvd->meta.f.m);
#endif
    kfree(pvd);
	}
}

struct pr_vma_data *create_and_add_file_pvd(struct file *filp)
{
	int i, cpu;
	struct pr_vma_data *pvd;

  pvd = kzalloc(sizeof(struct pr_vma_data), GFP_KERNEL);
  DMAP_BGON(pvd == NULL);
	
	for(i = 0; i < MAX_OPEN_FDS; ++i)
		pvd->open_fds[i] = NULL;

  pvd->type = D_FILE;
  pvd->dev_id = filp->f_inode->i_sb->s_dev;
  pvd->bk.filp = filp;
	pvd->open_fds[0] = filp;
  pvd->ino = filp->f_inode->i_ino;
  pvd->lower_page_mkwrite = NULL; // lower_page_mkwrite;
  pvd->is_readonly = true;
	pvd->is_mmap = false;
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

  atomic64_set(&pvd->cnt, 0);
  atomic64_set(&pvd->mmaped, 0);
  atomic64_set(&pvd->vma_count, 0);
	atomic64_set(&pvd->during_unlink, 0);
#if 0
	if(filp->f_inode->i_size >= PAGE_SIZE){
 		pvd->meta.f.m = (struct device_meta *)vmalloc((filp->f_inode->i_size >> PAGE_SHIFT) * sizeof(struct device_meta));
  	DMAP_BGON(pvd->meta.f.m == NULL);
  	memset(pvd->meta.f.m, 0, (filp->f_inode->i_size >> PAGE_SHIFT) * sizeof(struct device_meta));
  	pvd->meta.f.file_size = filp->f_inode->i_size;
	}else{
		pvd->meta.f.m = NULL;
  	pvd->meta.f.file_size = 0;
	}
#endif
	for(cpu = 0; cpu < 64; cpu++){
  	spin_lock_init(&pvd->tree_lock[cpu]);

#ifdef USE_RADIX_TREE_FOR_DIRTY
		INIT_RADIX_TREE(&pvd->dirty_tree[cpu], GFP_ATOMIC);
#else
  	pvd->dirty_tree[cpu] = RB_ROOT;
#endif
	}

	INIT_HLIST_NODE(&pvd->hchain);


	pvd->magic1 = PVD_MAGIC_1;
	pvd->magic2 = PVD_MAGIC_2;

	return pvd;
}

static ssize_t wrapfs_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
	int err;
	struct file *lower_file;
	struct dentry *dentry = file->f_path.dentry;
	struct pr_vma_data *pvd = NULL;
	struct tagged_page *tagged_page;

#ifdef USE_PERCPU_RADIXTREE
	const unsigned int cpus = num_online_cpus();
	unsigned int radix_tree_id;
#endif

	lower_file = wrapfs_lower_file(file);

	pvd = ((struct wrapfs_file_info*)file->private_data)->fmap_info->pvd;

	if(
		(pvd != NULL) 
		&& ( (pvd->magic1 == PVD_MAGIC_1) && (pvd->magic2 == PVD_MAGIC_2) ) 
		&& ( atomic64_read(&pvd->mmaped) == 1 )
	){

		pgoff_t pgoff_start = *ppos >> PAGE_SHIFT;
		pgoff_t pgoff_end = (*ppos + count) >> PAGE_SHIFT;
		
		if(io_is_direct(file)){
			/* 
			 * Direct I/O guideline:
			 *	- Configure a kiocb, iov_iter pair
			 *	- Call underlying fs read_iter to perform
			 *	  direct I/O from disk
			 *	- Scan the page range and copy fastmap
			 *	  page cache hits in iov_iter to replace stale
			 *	  device data
			 *	- Copy to buffer and return
			 */
			struct kiocb iocb;
			struct iov_iter iter;
			struct iovec iov = { .iov_base = buf, .iov_len = count };
			pgoff_t i;
			size_t starting_offset = *ppos - pgoff_start, 
			       ending_length = (*ppos + count) - pgoff_end;


			init_sync_kiocb(&iocb, lower_file);
			kiocb.ki_pos = *ppos;
			iov_iter_init(&iter, READ, &iov, 1, count);

			DMAP_BGON(lower_file->f_op->read_iter == NULL);

			err = lower_file->f_op->read_iter(&iocb, &iter);

			if(err < 0)
				goto read_done;

			if(pgoff_start == pgoff_end){
				rcu_read_lock();
#ifdef USE_PERCPU_RADIXTREE
				radix_tree_id = i % cpus;
				tagged_page = radix_tree_lookup(&(pvd->rdx[radix_tree_id]), pgoff_start);
#else
				tagged_page = radix_tree_lookup(&(pvd->rdx), pgoff_start);
#endif
				if(tagged_page != NULL)
					copy_page_to_iter(tagged_page->page, starting_offset, count, &iter);
				rcu_read_unlock();
				goto read_done;
			}

			for(i = pgoff_start; i <= pgoff_end; i++){
				rcu_read_lock();
#ifdef USE_PERCPU_RADIXTREE
				radix_tree_id = i % cpus;
				tagged_page = radix_tree_lookup(&(pvd->rdx[radix_tree_id]), i);
#else
				tagged_page = radix_tree_lookup(&(pvd->rdx), i);
#endif
				if(tagged_page != NULL){
					/* 
					 * Cache hit, copy to iov iterator (and consequently buf)
					 * Cases to preserve correct offsets:
					 *     1) pgoff_start: watch out for offset within initial page
					 *     2) pgoff_end: watch out for amount of bytes within last page
					 *     3) offset in between: just copy the entire page
					 * We also need to be careful if pgoff_start == pgoff_end in which
					 * case all the data we want is within a single page
					 */
					if(i == pgoff_start)
						copy_page_to_iter(tagged_page->page, starting_offset, PAGE_SIZE - starting_offset,
								&iter);
					else if(i == pgoff_end)
						copy_page_to_iter(tagged_page->page, 0, ending_length, &iter);
					else
						copy_page_to_iter(tagged_page->page, 0, PAGE_SIZE, &iter);
				}else{
					/* 
					 * Adjust iov iterator offset in order to copy
					 * fastmap cache hits to the correct offset within
					 * the iterator. This stems from the fact that while
					 * calls to copy_page_to_iter will advance the iov_iter
					 * offset, we still need to advance it on cache misses
					 */
					if(i == pgoff_start)
						iter.iov_offset += PAGE_SIZE - starting_offset;
					else if(i != pgoff_end)
						iter.iov_offset += PAGE_SIZE;
				}
				rcu_read_unlock();
			}
			goto read_done;
		}else{

			if(pgoff_start == pgoff_end){
				rcu_read_lock();
				
#ifdef USE_PERCPU_RADIXTREE
				radix_tree_id = pgoff_start % cpus;
				tagged_page = radix_tree_lookup(&(pvd->rdx[radix_tree_id]), pgoff_start);
#else
				tagged_page = radix_tree_lookup(&(pvd->rdx), pgoff_start);
#endif

				if(tagged_page != NULL){
					//printk(KERN_ERR "[%s:%s:%d] beg=%lld - end=%llu - readonly = %u pvd = %p ino = %lu\n", __FILE__, __func__, __LINE__, *ppos, *ppos + count, pvd->is_readonly, pvd, pvd->ino);

					/* TODO verify this using access_ok? */
					if(copy_to_user(buf, (const void *)(((char *)page_to_virt(tagged_page->page)) + *ppos), count)){
						printk(KERN_ERR "[%s:%s:%d] ERROR in copy_to_user()\n", __FILE__, __func__, __LINE__);
					}
					
					file_accessed(lower_file);
					file_accessed(file);
				
					rcu_read_unlock();
					return count;
				}
				rcu_read_unlock();
			}else{
				pgoff_t i;
				for(i = pgoff_start; i <= pgoff_end; i++){
					rcu_read_lock();
#ifdef USE_PERCPU_RADIXTREE
					radix_tree_id = i % cpus;
					tagged_page = radix_tree_lookup(&(pvd->rdx[radix_tree_id]), i);
#else
					tagged_page = radix_tree_lookup(&(pvd->rdx), i);
#endif
					if(tagged_page != NULL){
						printk(KERN_ERR "[%s:%s:%d] beg=%lld - end=%llu - readonly = %u pvd = %p ino = %lu\n", __FILE__, __func__, __LINE__, *ppos, *ppos + count, pvd->is_readonly, pvd, pvd->ino);
					}
					rcu_read_unlock();
				}
			}
		}
	}
	
	lower_file = wrapfs_lower_file(file);
	err = vfs_read(lower_file, buf, count, ppos);
	/* update our inode atime upon a successful lower read */
read_done:
	if (err >= 0)
		fsstack_copy_attr_atime(d_inode(dentry),
					file_inode(lower_file));

	return err;
}

static void wrapfs_file_release_cb(struct tagged_page *p)
{
	move_page_d2c_fifo_buffer_t(buf_data->banks, p);
}

static ssize_t wrapfs_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos)
{
	struct pr_vma_data *pvd = ((struct wrapfs_file_info*)file->private_data)->fmap_info->pvd;
	struct file *lower_file;
	struct dentry *dentry = file->f_path.dentry;
	int err = 0;

	if(
		(pvd != NULL) 
		&& ( (pvd->magic1 == PVD_MAGIC_1) && (pvd->magic2 == PVD_MAGIC_2) ) 
		&& ( atomic64_read(&pvd->mmaped) == 1 )
	){
		pgoff_t pgoff_start = *ppos >> PAGE_SHIFT;
		pgoff_t pgoff_end = (*ppos + count) >> PAGE_SHIFT;
		pgoff_t i;

		char *cur_buf = (char *)buf;
		size_t remain = count;
		unsigned long b = 0;
		unsigned long o = 0;

		if(count < PAGE_SIZE)
			printk(KERN_ERR "[%s:%s:%d] start = %lld -- end = %llu -- pvd->is_readonly = %u\n", __FILE__, __func__, __LINE__, *ppos, *ppos + count, pvd->is_readonly);

		if(pgoff_start == pgoff_end)
			printk(KERN_ERR "[%s:%s:%d] start = %lld -- end = %llu -- pvd->is_readonly = %u\n", __FILE__, __func__, __LINE__, *ppos, *ppos + count, pvd->is_readonly);

		if(io_is_direct(file)){
			/* 
			 * Direct I/O file writes
			 * Steps taken:
			 *	- Invalidate pages if in fastmap cache
			 *	- Call underlying file write code
			 */
			struct iovec iov = { .iov_base = (void __user *)buf, .iov_len = count };
			struct kiocb kiocb;
			struct iov_iter iter;

			for(i = pgoff_start; i <= pgoff_end; i++){
				struct tagged_page *tp;
				rcu_read_lock();
				tp = fmap_radix_lookup(pvd, i);
				if(tp == NULL){
					/* Cache miss, just continue */
					rcu_read_unlock();
					continue;
				}else{
					/* Cache hit, must invalidate */

					lpfifo_t *clean_queue = &buf_data->primary_fifo_data[tp->page->index % NUM_QUEUES];
					dfifo_t *dirty_queue = &buf_data->dirty_queue[(tp->page->index >> 9) % EVICTOR_THREADS];

					if(trylock_tp(tp, ULONG_MAX) != 0){ // failed to lock page
						rcu_read_unlock();
						i--; /* Decrement iterator to try again at the same offset */
						continue;
					}

					rcu_read_unlock();
					if (atomic_read(&tp->is_dirty) == 0) {
						unsigned int dirty_tree = tp->page->index % num_online_cpus();
						/* 
						 * Page is dirty, must remove from dirty queue, tree
						 * and drain
						 */
						spin_lock(&dirty_queue->dlock);
						dfifo_ops->remove(dirty_queue, tp);
						spin_unlock(&dirty_queue->dlock);

						spin_lock(&tp->pvd->tree_lock[dirty_tree]);
#ifdef USE_RADIX_TREE_FOR_DIRTY
						radix_tree_delete(&(tp->pvd->dirty_tree[dirty_tree]), tp->page->index);
#else
						tagged_rb_erase(&(tp->pvd->dirty_tree[dirty_tree]), tp->page->index);
#endif
						spin_unlock(&tp->pvd->tree_lock[dirty_tree]);

						drain_page(tp, buf_data, true);
						unlock_tp(tp);
					}else{
						/*
						 * Page is clean, remove from clean queue and radix tree
						 * Check this for possible optimizations, like avoiding 
						 * list removals and mapping clears
						 */
						spin_lock(&clean_queue->qlock);
						lpfifo_ops->remove(clean_queue, tp);
						spin_unlock(&clean_queue->qlock);

						drain_page(tp, buf_data, true);
						unlock_tp(tp);
					}
				}
			}
			lower_file = wrapfs_lower_file(file);

			/* Setup kiocb and iov iterator to call underlying write_iter */
			init_sync_kiocb(&kiocb, lower_file);
			kiocb.ki_pos = *ppos;
			iov_iter_init(&iter, WRITE, &iov, 1, count);

			DMAP_BGON(lower_file->f_op->write_iter == NULL);
			err = lower_file->f_op->write_iter(&kiocb, &iter);

			/* update our inode times+sizes upon a successful lower write */
			if (err >= 0) {
				fsstack_copy_inode_size(d_inode(dentry),
							file_inode(lower_file));
				fsstack_copy_attr_times(d_inode(dentry),
							file_inode(lower_file));
			}
			return err;
		}else{
			for(i = pgoff_start; i <= pgoff_end; i++){
				struct tagged_page *tagged_page;
				int rdx_ret;
				unsigned long page_byte_offset[2] = { 0 };
				struct tagged_page *tp[2] = { NULL };
				struct vm_area_struct *vma, __vma;

				vma = &__vma;
			
retry_find_page:	
				rcu_read_lock();
				tagged_page = fmap_radix_lookup(pvd, i);
				if(tagged_page == NULL){
					rcu_read_unlock();

					tagged_page = alloc_page_lock(&buf_data->banks->page_map, i, pvd);
					if(tagged_page == NULL){
						try_purge_pages_fast(buf_data->banks, 512, get_random_int() % NUM_QUEUES);
						io_schedule();
						goto retry_find_page;
					}

					if(trylock_tp(tagged_page, ULONG_MAX) != 0)
						DMAP_BGON(1); // it should always manage to lock the page

					rdx_ret = fmap_radix_insert(pvd, i, tagged_page);
					if(rdx_ret != 0)
						BUG();

					insert_page_fifo_buffer_t(buf_data->banks, tagged_page, false);

					vma->vm_file = file;
					page_byte_offset[0] = i << PAGE_SHIFT;
					tp[0] = tagged_page;

					if( (i == pgoff_start) || (i == pgoff_end) )  
						dmap_do_page_io(vma, pvd, tp, page_byte_offset, 1);
					
					dmap_do_tagged_page_writable(vma, pvd, tagged_page);
				}else if(tagged_page != NULL){
					if(trylock_tp(tagged_page, ULONG_MAX) != 0){ // failed to lock page
						rcu_read_unlock();
						goto retry_find_page;
					}

					rcu_read_unlock();
					if (atomic_read(&tagged_page->is_dirty) == 0) {
						vma->vm_file = file;
						dmap_do_tagged_page_writable(vma, pvd, tagged_page);
					}
				} //else if(tagged_page != NULL)

				b = 0;
				o = 0;

				if(i == pgoff_start){ // corner case -- only copy the required
					if (*ppos % PAGE_SIZE == 0) {
						b = (remain >= PAGE_SIZE)?(PAGE_SIZE):(remain);
					} else {
						if( (i * PAGE_SIZE) > *ppos )
							BUG();

						o = *ppos - (i * PAGE_SIZE);
						b = PAGE_SIZE - (unsigned long)o;
					}
				}else{
					b = (remain >= PAGE_SIZE)?(PAGE_SIZE):(remain);
				}

				if(copy_from_user((void *)((char *)page_to_virt(tagged_page->page) + o), (const void __user *)cur_buf, b))
					printk(KERN_ERR "[%s:%s:%d] ERROR in copy_from_user()\n", __FILE__, __func__, __LINE__);
					
				cur_buf += b;
				remain -= b;

				atomic_set(&tagged_page->page_valid, 1);
				unlock_tp(tagged_page);
			} // for
		} // if/else direct I/O
	} // if pvd

#if 0
//no_page:
	lower_file = wrapfs_lower_file(file);
	err = vfs_write(lower_file, buf, count, ppos);
	/* update our inode times+sizes upon a successful lower write */
	if (err >= 0) {
		fsstack_copy_inode_size(d_inode(dentry),
					file_inode(lower_file));
		fsstack_copy_attr_times(d_inode(dentry),
					file_inode(lower_file));
	}
#endif
	return count;
}

static int wrapfs_readdir(struct file *file, struct dir_context *ctx)
{
	int err;
	struct file *lower_file = NULL;
	struct dentry *dentry = file->f_path.dentry;

	lower_file = wrapfs_lower_file(file);
	err = iterate_dir(lower_file, ctx);
	file->f_pos = lower_file->f_pos;
	if (err >= 0)		/* copy the atime */
		fsstack_copy_attr_atime(d_inode(dentry),
					file_inode(lower_file));
	return err;
}

static long dmap_priority_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
#if 0
	int retval = 0;
	struct pr_vma_data *pvd = ((struct wrapfs_file_info*)file->private_data)->fmap_info->pvd;
	struct dmap_page_prio __user *__dpp_p;
	struct dmap_page_prio __dpp_d;
	
	if(pvd == NULL)
		return -EINVAL;
	
	DMAP_BGON( (pvd->magic1 != PVD_MAGIC_1) || (pvd->magic2 != PVD_MAGIC_2) );

	if(_IOC_TYPE(cmd) != DMAP_IOC_MAGIC)
		return -EINVAL;

  switch(cmd){
    case DMAP_SET_PAGE_PRIORITY: {
      __dpp_p = (struct dmap_page_prio __user *)arg;

      retval = copy_from_user((void *)&__dpp_d, __dpp_p, sizeof(struct dmap_page_prio));
      if(retval != 0)
        return -EFAULT;

			DMAP_BGON(pvd->meta.f.m == NULL);
      ((pvd->meta.f.m)[__dpp_d.pageno]).priority = __dpp_d.prio;
      break;
    }    
    case DMAP_CHANGE_PAGE_PRIORITY: {
      __dpp_p = (struct dmap_page_prio __user *)arg;

      retval = copy_from_user((void *)&__dpp_d, __dpp_p, sizeof(struct dmap_page_prio));
      if(retval != 0)
        return -EFAULT;

      change_page_priority(buf_data, __dpp_d.pageno, pvd, __dpp_d.prio);
      break;
    } 
		default:
			return -EINVAL;	
	}
#endif
	return 0;
}

static long wrapfs_unlocked_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	long err = -ENOTTY;
	struct file *lower_file;

	lower_file = wrapfs_lower_file(file);

	/* XXX: use vfs_ioctl if/when VFS exports it */
	if (!lower_file || !lower_file->f_op)
		goto out;

	err = dmap_priority_ioctl(file, cmd, arg);
	if (!err)
		goto out;

	if (lower_file->f_op->unlocked_ioctl)
		err = lower_file->f_op->unlocked_ioctl(lower_file, cmd, arg);

	/* some ioctls can change inode attributes (EXT2_IOC_SETFLAGS) */
	if (!err)
		fsstack_copy_attr_all(file_inode(file), file_inode(lower_file));
out:
	return err;
}

#ifdef CONFIG_COMPAT
static long wrapfs_compat_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	long err = -ENOTTY;
	struct file *lower_file;

	lower_file = wrapfs_lower_file(file);

	/* XXX: use vfs_ioctl if/when VFS exports it */
	if (!lower_file || !lower_file->f_op)
		goto out;
	
	err = dmap_priority_ioctl(file, cmd, arg);
	if (!err)
		goto out;

	if (lower_file->f_op->compat_ioctl)
		err = lower_file->f_op->compat_ioctl(lower_file, cmd, arg);

out:
	return err;
}
#endif

static int wrapfs_open(struct inode *inode, struct file *file)
{
	int i, err = 0;
	struct file *lower_file = NULL;
	struct path lower_path;
	//ktime_t t1, t2;
	
	//t1 = ktime_get();

	/* don't open unhashed/deleted files */
	if (d_unhashed(file->f_path.dentry)) {
		err = -ENOENT;
		goto out_err;
	}

	file->private_data = kzalloc(sizeof(struct wrapfs_file_info), GFP_KERNEL);
	if(!WRAPFS_F(file)){
		err = -ENOMEM;
		goto out_err;
	}

	((struct wrapfs_file_info *)file->private_data)->fmap_info = kzalloc(sizeof(struct fastmap_info), GFP_KERNEL);
	if(!((struct wrapfs_file_info *)file->private_data)->fmap_info){
		err = -ENOMEM;
		goto out_err;
	}

	/* open lower object and link wrapfs's file struct to lower's */
	wrapfs_get_lower_path(file->f_path.dentry, &lower_path);
	lower_file = dentry_open(&lower_path, file->f_flags, current_cred());
	path_put(&lower_path);

	if(S_ISREG(file->f_path.dentry->d_inode->i_mode))
	{
		struct pr_vma_data *pvd;
		int rdx_ret;

		rcu_read_lock();
		pvd = radix_tree_lookup(&ino_pvd_cache, lower_file->f_inode->i_ino);
		if(pvd == NULL){
			rcu_read_unlock();

			pvd = create_and_add_file_pvd(lower_file);
		
			DMAP_BGON(lower_file->f_inode->i_ino != pvd->ino);

			//printk(KERN_ERR "Creating new pvd for ino %lu with fp %p\n", pvd->ino, pvd->bk.filp);

			spin_lock(&ino_pvd_lock);
			rdx_ret = radix_tree_insert(&ino_pvd_cache, lower_file->f_inode->i_ino, pvd);
			spin_unlock(&ino_pvd_lock);
			DMAP_BGON(rdx_ret != 0);
		}else{
			rcu_read_unlock();
			
			for(i = 0; i < MAX_OPEN_FDS; ++i){
				if(pvd->open_fds[i] == NULL){
					pvd->open_fds[i] = lower_file;
					break;
				}
			}
				
			pvd->bk.filp = lower_file;
		}

		// set it to the private data of the file
		DMAP_BGON( (pvd->magic1 != PVD_MAGIC_1) || (pvd->magic2 != PVD_MAGIC_2) );
		((struct wrapfs_file_info*)file->private_data)->fmap_info->pvd = pvd;
	}

	if (IS_ERR(lower_file)) {
		err = PTR_ERR(lower_file);
		lower_file = wrapfs_lower_file(file);
		if (lower_file) {
			wrapfs_set_lower_file(file, NULL);
			fput(lower_file); /* fput calls dput for lower_dentry */
		}
	} else {
		wrapfs_set_lower_file(file, lower_file);
	}

	if (err)
		kfree(WRAPFS_F(file));
	else
		fsstack_copy_attr_all(inode, wrapfs_lower_inode(inode));

out_err:
	//t2 = ktime_get();
	//atomic64_add(ktime_to_us(ktime_sub(t2, t1)), &ms_in_open);

	return err;
}

static int wrapfs_flush(struct file *file, fl_owner_t id)
{
	int err = 0;
	struct file *lower_file = NULL;

	lower_file = wrapfs_lower_file(file);
	if (lower_file && lower_file->f_op && lower_file->f_op->flush) {
		filemap_write_and_wait(file->f_mapping);
		err = lower_file->f_op->flush(lower_file, id);
	}

	return err;
}



static void wrapfs_file_fsync_cb(struct tagged_page *p)
{
	clear_mappings(p, NULL);
	move_page_d2c_fifo_buffer_t(buf_data->banks, p);
}

/* release all lower object references & free the file info structure */
static int wrapfs_file_release(struct inode *inode, struct file *file)
{
	int i;
	struct file *lower_file;
	struct pr_vma_data *pvd;
#ifdef USE_PERCPU_RADIXTREE
	int cpu;
#endif

	lower_file = wrapfs_lower_file(file);

	pvd = ((struct wrapfs_file_info*)file->private_data)->fmap_info->pvd;
	if(pvd != NULL && pvd->is_mmap == true && pvd->is_valid == true && atomic64_read(&pvd->mmaped) == 1){
    struct tagged_page *p;
    void **slot;
    struct radix_tree_iter iter;
    pgoff_t start = 0;
    uint64_t num_frees = 0;

		if(lower_file)
			((struct wrapfs_file_info*)file->private_data)->fmap_info->pvd->bk.filp = lower_file;
		else
			DMAP_BGON(1);

		write_dirty_pages(pvd, wrapfs_file_release_cb);

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

		for(i = 0; i < MAX_OPEN_FDS; ++i){
			if(pvd->open_fds[i] == lower_file){
				pvd->open_fds[i] = NULL;
				break;
			}
		}

		for(i = 0; i < MAX_OPEN_FDS; ++i){
			if(pvd->open_fds[i] != NULL && pvd->open_fds[i]->f_inode == NULL){
				pvd->open_fds[i] = NULL;
			}
		}

		for(i = 0; i < MAX_OPEN_FDS; ++i){
			if(pvd->open_fds[i] != NULL && pvd->open_fds[i]->f_inode != NULL){
				((struct wrapfs_file_info*)file->private_data)->fmap_info->pvd->bk.filp = pvd->open_fds[i];
				break;
			}
		}

		((struct wrapfs_file_info*)file->private_data)->fmap_info->pvd = NULL;
	}else if(pvd != NULL){
		for(i = 0; i < MAX_OPEN_FDS; ++i)
			if(pvd->open_fds[i] == lower_file){
				pvd->open_fds[i] = NULL;
				break;
			}
	}

	if(lower_file){
		wrapfs_set_lower_file(file, NULL);
		fput(lower_file);
	}

	kfree(WRAPFS_F(file));
	return 0;
}

static int wrapfs_fsync(struct file *file, loff_t start, loff_t end, int datasync)
{
#if 0
	int err;
	struct file *lower_file;
	struct path lower_path;
	struct dentry *dentry = file->f_path.dentry;
#endif
	struct pr_vma_data *pvd;

	pvd = ((struct wrapfs_file_info*)file->private_data)->fmap_info->pvd;
	if(pvd == NULL)
		goto mmap_fsync;
	
	DMAP_BGON( (pvd->magic1 != PVD_MAGIC_1) || (pvd->magic2 != PVD_MAGIC_2) );
	DMAP_BGON(pvd->type == D_INVD);
	DMAP_BGON(pvd->type == D_BLKD);

	write_dirty_pages(pvd, wrapfs_file_fsync_cb);

mmap_fsync:
#if 0
	err = __generic_file_fsync(file, start, end, datasync);
	if(err)
		goto out;

	lower_file = wrapfs_lower_file(file);
	wrapfs_get_lower_path(dentry, &lower_path);
	err = vfs_fsync_range(lower_file, start, end, datasync);
	wrapfs_put_lower_path(dentry, &lower_path);
	
out:
#endif
	return 0;
}

static int wrapfs_fasync(int fd, struct file *file, int flag)
{
	int err = 0;
	struct file *lower_file = NULL;

	lower_file = wrapfs_lower_file(file);
	if (lower_file->f_op && lower_file->f_op->fasync)
		err = lower_file->f_op->fasync(fd, lower_file, flag);

	return err;
}

/*
 * Wrapfs cannot use generic_file_llseek as ->llseek, because it would
 * only set the offset of the upper file.  So we have to implement our
 * own method to set both the upper and lower file offsets
 * consistently.
 */
static loff_t wrapfs_file_llseek(struct file *file, loff_t offset, int whence)
{
	int err;
	struct file *lower_file;

	err = generic_file_llseek(file, offset, whence);
	if (err < 0)
		goto out;

	lower_file = wrapfs_lower_file(file);
	err = generic_file_llseek(lower_file, offset, whence);

out:
	return err;
}

/*
 * Wrapfs read_iter, redirect modified iocb to lower read_iter
 */
ssize_t
wrapfs_read_iter(struct kiocb *iocb, struct iov_iter *iter)
{
	int err;
	struct file *file = iocb->ki_filp, *lower_file;

#ifdef DEBUG_DIRECT_RW
	struct pr_vma_data *pvd = ((struct wrapfs_file_info*)file->private_data)->fmap_info->pvd;
	if(pvd != NULL){
		DMAP_BGON( (pvd->magic1 != PVD_MAGIC_1) || (pvd->magic2 != PVD_MAGIC_2) );
		WARN(atomic64_read(&pvd->cnt) > 0, "[%s:%s:%d][%ld]\n", __FILE__, __func__, __LINE__, atomic64_read(&pvd->cnt));
	}
#endif

	printk(KERN_ERR "[%s:%s:%d]\n", __FILE__, __func__, __LINE__);

	lower_file = wrapfs_lower_file(file);
	if (!lower_file->f_op->read_iter) {
		err = -EINVAL;
		goto out;
	}

	get_file(lower_file); /* prevent lower_file from being released */
	iocb->ki_filp = lower_file;
	err = lower_file->f_op->read_iter(iocb, iter);
	iocb->ki_filp = file;
	fput(lower_file);
	/* update upper inode atime as needed */
	if (err >= 0 || err == -EIOCBQUEUED)
		fsstack_copy_attr_atime(d_inode(file->f_path.dentry),
					file_inode(lower_file));
out:
	return err;
}

/*
 * Wrapfs write_iter, redirect modified iocb to lower write_iter
 */
ssize_t
wrapfs_write_iter(struct kiocb *iocb, struct iov_iter *iter)
{
	int err;
	struct file *file = iocb->ki_filp, *lower_file;

#ifdef DEBUG_DIRECT_RW
	struct pr_vma_data *pvd = ((struct wrapfs_file_info*)file->private_data)->fmap_info->pvd;
	if(pvd != NULL){
		DMAP_BGON( (pvd->magic1 != PVD_MAGIC_1) || (pvd->magic2 != PVD_MAGIC_2) );
		WARN(atomic64_read(&pvd->cnt) > 0, "[%s:%s:%d][%ld]\n", __FILE__, __func__, __LINE__, atomic64_read(&pvd->cnt));
	}
#endif

//	struct pr_vma_data *pvd = ((struct wrapfs_file_info*)file->private_data)->fmap_info->pvd;
//	if(
//		(pvd != NULL) 
//		&& ( (pvd->magic1 == PVD_MAGIC_1) && (pvd->magic2 == PVD_MAGIC_2) ) 
//		&& ( atomic64_read(&pvd->mmaped) == 1 )
//	){
		printk(KERN_ERR "[%s:%s:%d] iocb->ki_pos = %lld, iter->type = %d, iter->iov_offset = %zu, iter->count = %zu\n", 
			__FILE__, 
			__func__, 
			__LINE__,
			iocb->ki_pos,
			iter->type,
			iter->iov_offset,
			iter->count
		);
//	}

	lower_file = wrapfs_lower_file(file);
	if (!lower_file->f_op->write_iter) {
		err = -EINVAL;
		goto out;
	}

	get_file(lower_file); /* prevent lower_file from being released */
	iocb->ki_filp = lower_file;
	err = lower_file->f_op->write_iter(iocb, iter);
	iocb->ki_filp = file;
	fput(lower_file);
	/* update upper inode times/sizes as needed */
	if (err >= 0 || err == -EIOCBQUEUED) {
		fsstack_copy_inode_size(d_inode(file->f_path.dentry),
					file_inode(lower_file));
		fsstack_copy_attr_times(d_inode(file->f_path.dentry),
					file_inode(lower_file));
	}
out:
	return err;
}

static long wrapfs_fallocate(struct file *file, int mode, loff_t offset, loff_t len)
{
	int err = 0;
	struct file *lower_file = NULL;

	lower_file = wrapfs_lower_file(file);
	err = vfs_fallocate(lower_file, mode, offset, len);
	if (err >= 0) {
		fsstack_copy_inode_size(d_inode(file->f_path.dentry),
					file_inode(lower_file));
		fsstack_copy_attr_times(d_inode(file->f_path.dentry),
					file_inode(lower_file));
	}

	return err;
}

extern int dmap_mmap_file(struct file *filp, struct vm_area_struct *vma);

const struct file_operations wrapfs_main_fops = {
	.llseek					= generic_file_llseek,
	.read						= wrapfs_read,
	.write					= wrapfs_write,
	.unlocked_ioctl	= wrapfs_unlocked_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl		= wrapfs_compat_ioctl,
#endif
	.mmap						= dmap_mmap_file,
	.open						= wrapfs_open,
	.flush					= wrapfs_flush,
	.release				= wrapfs_file_release,
	.fsync					= wrapfs_fsync,
	.fasync					= wrapfs_fasync,
	.fallocate			= wrapfs_fallocate,
	.read_iter			= wrapfs_read_iter,
	.write_iter			= wrapfs_write_iter,
};

/* trimmed directory options */
const struct file_operations wrapfs_dir_fops = {
	.llseek					= wrapfs_file_llseek,
	.read						= generic_read_dir,
	.iterate				= wrapfs_readdir,
	.unlocked_ioctl	= wrapfs_unlocked_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl		= wrapfs_compat_ioctl,
#endif
	.open						= wrapfs_open,
	.release				= wrapfs_file_release,
	.flush					= wrapfs_flush,
	.fsync					= wrapfs_fsync,
	.fasync					= wrapfs_fasync,
};
