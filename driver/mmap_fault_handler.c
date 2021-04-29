#include <linux/module.h>
#include <linux/mm.h>	/* everything */
#include <linux/errno.h> /* error codes */
#include <linux/delay.h>
#include <linux/time.h>
#include <linux/pagemap.h>
#include <linux/blkdev.h>
#include <linux/version.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/major.h>
#include <linux/backing-dev.h>
#include <linux/moduleparam.h>
#include <linux/capability.h>
#include <linux/uio.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/gfp.h>
#include <linux/compat.h>
#include <linux/vmalloc.h>
#include <linux/delay.h>
#include <linux/bio.h>
#include <linux/random.h>
#include <linux/buffer_head.h>
#include <linux/pfn_t.h>
#include <linux/huge_mm.h>
#include <linux/rmap.h>
#include <linux/init.h>

#include <asm/pgtable.h>
#include <asm/segment.h>
#include <asm/uaccess.h>
#include <asm/tlbflush.h>

#include "dmap.h"
#include "wrapfs.h"
#include "mmap_fault_handler.h"
#include "mmap_buffer_interface.h"
#include "shared_defines.h"
#include "mmap_buffer_rbtree.h"

/* Global and loadable variables - do not initialize inreset_mmap_params, they are controlled by module load params*/
extern buf_ld_params_t *g_buf_ld_params;

#ifdef USE_PROC_SYS_VM_COUNTERS
extern atomic_t mm_lf_stat_pgfaults;
extern atomic_t mm_lf_stat_pgfaults_minor;
extern atomic_t mm_lf_stat_pgfaults_major;
extern atomic_t mm_lf_stat_mkwrites;
extern atomic_t mm_lf_stat_map_pages;
extern atomic_t mm_lf_stat_mapped_readahead_pgs;
extern atomic_t mm_lf_stat_failed_mapped_readahead_pgs;

extern atomic_t mm_lf_stat_fail_maps;
extern atomic_t mm_lf_stat_succ_maps;
extern atomic_t mm_lf_stat_call_maps;

#ifdef USE_PAGE_FAULT_FASTPATH
extern atomic_t mm_lf_pgfault_slow;
extern atomic_t mm_lf_pgfault_fast;
extern atomic_t mm_lf_pgfault_fast_failed_pte;
extern atomic_t mm_lf_pgfault_fast_failed_cas;
#endif
#endif

extern void try_purge_pages(fifo_buffer_t *buf, unsigned long target, int queue_id);
extern unsigned int try_purge_pages_fast(fifo_buffer_t *buf, unsigned int N, int qid);

extern void remove_mappings(struct tagged_page *p, struct evictor_tlb *etlb);

extern void write_bio_tagged_page(struct tagged_page *evicted_page);
extern void write_file_tagged_page(struct tagged_page *evicted_page);

extern void clear_fifo_entry(struct tagged_page *tagged_page_to_clear, struct pr_vma_rmap *pvr, bool flush_tlb);

#ifdef USE_HUGEPAGES
extern struct page *hugepages[16];
extern spinlock_t hugepages_lock;
extern unsigned long hugepages_idx;
#endif

extern atomic64_t ms_in_get_page;

/* MMAP interface to buffer */
banked_buffer_t *buf_data;

extern struct kmem_cache *pve_alloc_cache;

int reset_mmap_params(buf_ld_params_t *buf_ld_params)
{
	reset_params(buf_data);
	return init_mmap_params(buf_ld_params);
}

int init_mmap_params(buf_ld_params_t *ld_params)
{
	int err = init_params(buf_data, ld_params);
	if (err)
		printk(KERN_ERR "mmap_fault_handler failed to initialize mmap parameters err=%d\n", err);

	return err;
}

void cleanup_mmap_params()
{
	cleanup_params(buf_data);
}

int update_mmap_runtime_parameters(mmap_buffer_t *buf, buf_rt_params_t *rt_params)
{
	return update_runtime_parameters(buf, rt_params);
}

void reset_mmap_state()
{
	reset_state(buf_data);
}

void reset_mmap_history()
{
	reset_history(buf_data);
}

void check_file_size(struct pr_vma_data *pvd)
{
#if 0
	bool dentry_opened = false;

	if (pvd->type == D_BLKD)
		return;

	if (pvd->is_mmap == false)
		return;

	if(pvd->bk.filp == NULL){
		pvd->bk.filp = dentry_open(&pvd->fpath, pvd->fflags, current_cred());
		DMAP_BGON(IS_ERR(pvd->bk.filp));
		dentry_opened = true;
	}

	DMAP_BGON( pvd->bk.filp == NULL );
	DMAP_BGON( pvd->bk.filp->f_inode == NULL );

	if (pvd->meta.f.file_size != pvd->bk.filp->f_inode->i_size)
	{	
		unsigned long sz = pvd->bk.filp->f_inode->i_size >> PAGE_SHIFT;
		struct device_meta *__pm;

		if(sz == 0)
			sz++;

		__pm = vmalloc(sz * sizeof(struct device_meta));
		DMAP_BGON(__pm == NULL);
		memset(__pm, 0, sz * sizeof(struct device_meta));
		memcpy(__pm, pvd->meta.f.m, (((pvd->meta.f.file_size > pvd->bk.filp->f_inode->i_size) ? (pvd->bk.filp->f_inode->i_size) : (pvd->meta.f.file_size)) >> PAGE_SHIFT) * sizeof(struct device_meta));
		vfree(pvd->meta.f.m);
		pvd->meta.f.m = __pm;
		pvd->meta.f.file_size = pvd->bk.filp->f_inode->i_size;
	}

	if(dentry_opened){
		fput(pvd->bk.filp);
		dentry_opened = false;
	}
#endif
}

static int read_pages_bio(struct block_device *device, unsigned long *byte_offset, struct tagged_page **tagged_page, int num_pages)
{
	unsigned char __tmp[BIO_SPAGE_SIZE];
	struct bio *bio = (struct bio *)__tmp;
	int i;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,9,0) && LINUX_VERSION_CODE < KERNEL_VERSION(4,10,0)
	bio_init(bio);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(4,14,0) && LINUX_VERSION_CODE < KERNEL_VERSION(4,15,0)
	bio_init(bio, bio->bi_inline_vecs, num_pages);
#endif

	bio->bi_pool = NULL;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,9,0) && LINUX_VERSION_CODE < KERNEL_VERSION(4,10,0)
	bio->bi_bdev = (struct block_device *)device;
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(4,14,0) && LINUX_VERSION_CODE < KERNEL_VERSION(4,15,0)
	bio_set_dev(bio, (struct block_device *)device);
#endif
	bio_set_op_attrs(bio, REQ_OP_READ, 0);
	bio->bi_iter.bi_sector = byte_offset[0] >> 9;

	for(i = 0; i < num_pages; ++i)
		if(bio_add_page(bio, tagged_page[i]->page, PAGE_SIZE, 0) != PAGE_SIZE){
			printk(KERN_ERR "%s:%s:%d\n", __FILE__, __func__, __LINE__);
			return 1;
		}

	if(submit_bio_wait(bio) != 0){
		printk(KERN_ERR "%s:%s:%d\n", __FILE__, __func__, __LINE__);
		return 1;
	}

	return 0;
}

static int read_or_fake_page_bio(struct pr_vma_data *pvd, unsigned long *page_byte_offset, struct tagged_page **tagged_page, int num_pages)
{
	int i, tb = 0;
	struct block_device *device;
	struct raw_device_data *device_metadata;

	device = pvd->bk.bdev;
	device_metadata = pvd->meta.rdd;

	DMAP_BGON(num_pages > 1);
	DMAP_BGON(tagged_page[0]->pvd->type != D_BLKD);
	DMAP_BGON(tagged_page[0]->pvd->bk.bdev != device);
	DMAP_BGON(tagged_page[0]->pvd->ino != 0);
	DMAP_BGON(tagged_page[0]->pvd != pvd);

	for(i = 0; i < num_pages; ++i)
		tb |= constant_test_bit(page_byte_offset[i] / 4096, device_metadata->bitmap.bitmap);

	if(tb == 1){
		if(read_pages_bio(device, page_byte_offset, tagged_page, num_pages))
			return 1;
	}else{ /* fake read */
		for(i = 0; i < num_pages; ++i){
			set_bit(page_byte_offset[i] / 4096, device_metadata->bitmap.bitmap);
		}
	}

	return 0;
}

static int read_pages_file(struct vm_area_struct *vma, struct pr_vma_data *pvd, unsigned long *byte_offset, struct tagged_page **tagged_page, int num_pages)
{
	ssize_t rd_retval;
	struct file *lower_file = wrapfs_lower_file(vma->vm_file);
	loff_t pos = byte_offset[0];

#ifdef USE_DIRECT_IO
	int pgid;
	struct bio_vec bvec[PAGES_PER_PGFAULT];
	struct iov_iter iter;
	//ssize_t (*direct_IO)(struct kiocb *, struct iov_iter * iter) = lower_file->f_inode->i_mapping->a_ops->direct_IO;
	ssize_t (*read_iter) (struct kiocb *, struct iov_iter *) = lower_file->f_op->read_iter;

	struct kiocb iocb = {
		.ki_filp = lower_file,
		.ki_pos = pos,
		.ki_complete = NULL,
		.private = NULL,
		.ki_flags = IOCB_DIRECT,
	};

	for(pgid = 0; pgid < num_pages; pgid++){
		bvec[pgid].bv_page = tagged_page[pgid]->page;
		bvec[pgid].bv_len = PAGE_SIZE;
		bvec[pgid].bv_offset = 0;
	}

	iter.type = READ | ITER_BVEC;
	iter.iov_offset = 0;
	iter.count = num_pages * PAGE_SIZE;
	iter.bvec = bvec;
	iter.nr_segs = num_pages;

	//get_file(lower_file); /* prevent lower_file from being released */

	//rd_retval = direct_IO(&iocb, &iter);
	rd_retval = read_iter(&iocb, &iter);
	if(rd_retval > 0)
		rd_retval /= PAGE_SIZE;

	if(rd_retval < 0){
		printk(KERN_ERR "[%s:%s:%d] direct_IO(READ, ...) returns %zd file size %lld bytes\n", __FILE__, __func__, __LINE__, rd_retval, wrapfs_lower_file(vma->vm_file)->f_inode->i_size);
		fput(lower_file);
		return -1;
	}

	//fput(lower_file);
#else

#if PAGES_PER_PGFAULT != 1
#error "Using more than one pages is not supported in kernel_read()"
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,9,0) && LINUX_VERSION_CODE < KERNEL_VERSION(4,10,0)
	rd_retval = kernel_read(lower_file, pos, (char *)page_address(tagged_page->page), PAGE_SIZE);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(4,14,0) && LINUX_VERSION_CODE < KERNEL_VERSION(4,15,0)
	rd_retval = kernel_read(lower_file, page_address(tagged_page->page), PAGE_SIZE, &pos);
#endif
	if(rd_retval != PAGE_SIZE){
		printk(KERN_ERR "[%s:%s:%d] kernel_read(...) returns %zd\n", __FILE__, __func__, __LINE__, rd_retval);
		return 1;
	}

#endif

	return rd_retval;
}

static void dmap_set_page_priority(struct pr_vma_data *pvd, struct tagged_page *tagged_page, unsigned long page_offset)
{
	if (pvd->type == D_BLKD)
		tagged_page->priority = pvd->meta.rdd->meta[page_offset].priority;
#if 0
	else // D_FILE
		tagged_page->priority = pvd->meta.f.m[page_offset].priority;
#endif
}

static void dmap_do_page_io(struct vm_area_struct *vma, struct pr_vma_data *pvd, struct tagged_page **tagged_page, unsigned long *page_byte_offset, int num_pages)
{	
	int pgid, npages = 0;

	if(atomic_read(&tagged_page[0]->page_valid) == 0){
		if(pvd->type == D_BLKD)
			npages = read_or_fake_page_bio(pvd, page_byte_offset, tagged_page, num_pages);
		else // D_FILE
			npages = read_pages_file(vma, pvd, page_byte_offset, tagged_page, num_pages);

		DMAP_BGON(npages < 0);
	}

	for(pgid = 0; pgid < num_pages; pgid++){
		if(atomic_read(&tagged_page[pgid]->page_valid) == 0){
			atomic_set(&tagged_page[pgid]->page_valid, 1);
#ifdef USE_WAITQUEUE_FOR_COLLISIONS
			wake_up_interruptible_all(&tagged_page[pgid]->waiters);
#endif
		}
	}

	//DMAP_BGON(num_pages != npages);
}

/*
 * off is used only for prefetching. In all other cases this should be 0.
 * Is is in number of pages beyond vmf->address.
 * (addr + (off * PAGE_SIZE))
 */
static void dmap_add_or_update_rmap(struct vm_fault *vmf, struct pr_vma_data *pvd, struct tagged_page *tagged_page, int off)
{
	struct pr_vma_entry *pve = NULL;
	struct pr_vma_rmap *pvr = NULL;
	bool found = false;
	int i, cpu;
	int free_entry = -1;
	struct vm_area_struct *vma = vmf->vma;
	struct fastmap_info *fmap_info = (struct fastmap_info *)vma->vm_private_data;

	pve = fmap_info->pve;

	spin_lock(&tagged_page->rmap_lock);
	for(i = 0; i < MAX_RMAPS_PER_PAGE; i++){
		if(pvr_is_valid(&tagged_page->rmap[i])){
			if(pvr_get_vma(&tagged_page->rmap[i]) == vma){
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,9,0) && LINUX_VERSION_CODE < KERNEL_VERSION(4,10,0)
				if(pvr_get_vaddr(&tagged_page->rmap[i]) != (unsigned long)(vmf->virtual_address + (off * PAGE_SIZE))){
					printk(KERN_ERR "[%lu != %lu]\n", pvr_get_vaddr(&tagged_page->rmap[i]), (unsigned long)(vmf->virtual_address + (off * PAGE_SIZE)));
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(4,14,0) && LINUX_VERSION_CODE < KERNEL_VERSION(4,15,0)
				if(pvr_get_vaddr(&tagged_page->rmap[i]) != (vmf->address + (off * PAGE_SIZE))){
					printk(KERN_ERR "[%lu != %lu]\n", pvr_get_vaddr(&tagged_page->rmap[i]), vmf->address + (off * PAGE_SIZE));
#endif
					DMAP_BGON(1);
				}
				found = true;
				break;
			}
		}else{
			free_entry = i;
		}
	}

	if(!found){
		DMAP_BGON(free_entry == -1);

		pvr = &(tagged_page->rmap[free_entry]);

		pvr_set_idx(pvr, free_entry);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,9,0) && LINUX_VERSION_CODE < KERNEL_VERSION(4,10,0)
		pvr_set_vaddr(pvr, (unsigned long)vmf->virtual_address + (off * PAGE_SIZE));
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(4,14,0) && LINUX_VERSION_CODE < KERNEL_VERSION(4,15,0)
		pvr_set_vaddr(pvr, vmf->address + (off * PAGE_SIZE));
#endif
		pvr_set_vma(pvr, vma);
		pvr_set_cpu(pvr, 0);
		pvr_mk_valid(pvr);
	}
	spin_unlock(&tagged_page->rmap_lock);

	if(!found){
		DMAP_BGON(pvr == NULL);
		DMAP_BGON(pve == NULL);

		cpu = smp_processor_id();
		spin_lock(&pve->pcpu_mapped_lock[cpu]);
		list_add_tail(&pvr->vma_mapped, &pve->pcpu_mapped[cpu]);
		pvr_set_cpu(pvr, cpu);
		spin_unlock(&pve->pcpu_mapped_lock[cpu]);
	}
}

void dmap_do_tagged_page_writable(struct vm_area_struct *vma, struct pr_vma_data *pvd, struct tagged_page *tagged_page)
{
	int retval;
	const unsigned int dirty_tree_id = tagged_page->page->index % num_online_cpus();
	
	/*
	 * This is the case where 2 (or more threads) call mkwrite concurently.
	 * The first acqires the lock and makes the page writable. Then it releases
	 * the lock and the second calls this function for a second time. We should
	 * not do anything in this case. atomic_cmpxchg is not required as we have 
	 * the page lock.
	 */
	if(atomic_cmpxchg(&tagged_page->is_dirty, 0, 1) != 0)
		return;

	/*
	 * We are going to make a tagged_page writable. Except from everything 
	 * else we have also to move this page from clean queue to dirty queue.
	 * So we have to **ENSURE** that this page is in the clean queue.
	 */
	if((retval = atomic_read(&tagged_page->buffer_id)) != BI_CLEAN){
		printk(KERN_ERR 
			"buffer_id = [%d] is_dirty = [%d] is_valid = [%d] offset = [%lu]\n", 
			retval, 
			atomic_read(&tagged_page->is_dirty), 
			atomic_read(&tagged_page->page_valid), 
			tagged_page->page->index 
		);
		print_tp(tagged_page);
		BUG();
	}
	
	pvd->is_readonly = false;
	
	spin_lock(&pvd->tree_lock[dirty_tree_id]);
#ifdef USE_RADIX_TREE_FOR_DIRTY
	{
		int ret = radix_tree_insert(&pvd->dirty_tree[dirty_tree_id], tagged_page->page->index, tagged_page);
		BUG_ON(ret != 0);
	}
#else
	tagged_rb_insert(&pvd->dirty_tree[dirty_tree_id], tagged_page);
#endif
	spin_unlock(&pvd->tree_lock[dirty_tree_id]);
	
	move_page_c2d_fifo_buffer_t(buf_data->banks, tagged_page);
	SetPageDirty(tagged_page->page);

	if(tagged_page->pvd->type == D_FILE)
		file_update_time(wrapfs_lower_file(vma->vm_file));
}

static struct tagged_page *fmap_radix_lookup(struct pr_vma_data *pvd, unsigned long offset)
{
#ifdef USE_PERCPU_RADIXTREE
    const unsigned int cpus = num_online_cpus();
    const unsigned int radix_tree_id = offset % cpus;
    return radix_tree_lookup(&(pvd->rdx[radix_tree_id]), offset);
#else
    return radix_tree_lookup(&(pvd->rdx), offset);
#endif
}

static int fmap_radix_insert(struct pr_vma_data *pvd, unsigned long offset, struct tagged_page *p)
{
    int ret = -1;
#ifdef USE_PERCPU_RADIXTREE
    const unsigned int cpus = num_online_cpus();
    const unsigned int radix_tree_id = offset % cpus;

    spin_lock(&pvd->radix_lock[radix_tree_id]);
    ret = radix_tree_insert(&pvd->rdx[radix_tree_id], offset, p);
    spin_unlock(&pvd->radix_lock[radix_tree_id]);
#else
    spin_lock(&pvd->radix_lock);
    ret = radix_tree_insert(&pvd->rdx, offset, p);
    spin_unlock(&pvd->radix_lock);
#endif
    return ret;
}

int perma_getpage(struct vm_area_struct *vma, struct page **pagep, struct vm_fault *vmf)
{
	unsigned long page_offset[PAGES_PER_PGFAULT];
	unsigned long page_byte_offset[PAGES_PER_PGFAULT];
	struct tagged_page *tagged_page[PAGES_PER_PGFAULT];

	fifo_buffer_t *bank;
	struct pr_vma_data *pvd;
	int rdx_ret;
	int pgid = 0, num_pages, dev_pgs;

	for(pgid = 0; pgid < PAGES_PER_PGFAULT; pgid++)
		tagged_page[pgid] = NULL;

	pvd = ((struct fastmap_info *)vma->vm_private_data)->pvd
	DMAP_BGON(pvd == NULL);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,9,0) && LINUX_VERSION_CODE < KERNEL_VERSION(4,10,0)
	if(unlikely((unsigned long)vmf->virtual_address < vma->vm_start || (unsigned long)vmf->virtual_address >= vma->vm_end))
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(4,14,0) && LINUX_VERSION_CODE < KERNEL_VERSION(4,15,0)
	if(unlikely(vmf->address < vma->vm_start || vmf->address >= vma->vm_end))
#endif
		return -EFAULT;

	check_file_size(pvd);

	DMAP_BGON(pvd == NULL);

	dev_pgs = (pvd->type == D_FILE)?(wrapfs_lower_file(vma->vm_file)->f_inode->i_size >> PAGE_SHIFT):(get_capacity(pvd->bk.bdev->bd_disk) >> 3);

	num_pages = ( (vmf->pgoff + PAGES_PER_PGFAULT) <= dev_pgs )?(PAGES_PER_PGFAULT):(dev_pgs - vmf->pgoff);
	if(num_pages == 0)
		num_pages++;

	// vmf->pgoff -- number of pages from the device beginning
	// vmf->address -- address of fault in bytes page aligned and should be normalized
	// [ 1651.575290] pg_fault :: vmf->vma->vm_start = 0x7f85fa4fd000, vmf->vma->vm_end = 0x7fc198b53000
	// [ 1651.594164] pg_fault :: vmf->pgoff = 0x3, vmf->address = 0x7f85fa500000

	for(pgid = 0; pgid < num_pages; pgid++)
		page_offset[pgid] = vmf->pgoff + pgid; /* page_offset is a number of pages */

	bank = buf_data->banks;

	for(pgid = 0; pgid < num_pages; pgid++){
retry_find_page:
		rcu_read_lock();
		tagged_page[pgid] = fmap_radix_lookup(pvd, page_offset[pgid]);
		if(tagged_page[pgid] == NULL) 
		{
			rcu_read_unlock();
			tagged_page[pgid] = alloc_page_lock(&bank->page_map, page_offset[pgid], pvd);
			if(tagged_page[pgid] == NULL){
				/* int num_ev = */ try_purge_pages_fast(bank, 512, get_random_int() % NUM_QUEUES);; /* XXX tunable */
				// if(num_ev == 0)
				//	printk(KERN_ERR "Cannor evict any pages\n");
				io_schedule();
				goto retry_find_page;
			}

			DMAP_BGON(locked_tp(tagged_page[pgid]));
			
			//while(trylock_tp(tagged_page[pgid], ULONG_MAX) != 0) // failed to lock page
			//	wait_on_page_bit(tagged_page[pgid]->page, PG_locked);
			if(trylock_tp(tagged_page[pgid], ULONG_MAX) != 0)
				DMAP_BGON(1); // it should always manage to lock the page
			
			DMAP_BGON(atomic_read(&tagged_page[pgid]->in_use) == 0);

			rdx_ret = fmap_radix_insert(pvd, page_offset[pgid], tagged_page[pgid]);
			if(rdx_ret == 0){ // fine!
				/* here the page is locked */
#ifdef USE_PROC_SYS_VM_COUNTERS
				atomic_inc(&mm_lf_stat_pgfaults_major);
#endif
			}else if(rdx_ret == -EEXIST){ /* Someone else already inserted the page! */
				free_page_lock(&bank->page_map, tagged_page[pgid]); /* it requires the page locked */
				goto retry_find_page;
			}else{ /* why do we have an error? */
				DMAP_BGON(1);
			}
		}else{ // if(tagged_page[pgid] != NULL)

			if(trylock_tp(tagged_page[pgid], ULONG_MAX) != 0){ // failed to lock page
				rcu_read_unlock();
				wait_on_page_bit(tagged_page[pgid]->page, PG_locked);
				goto retry_find_page;
			}

			rcu_read_unlock();

			DMAP_BGON(atomic_read(&tagged_page[pgid]->in_use) == 0);

			if(pgid == 0){ /* we have a hit for the page we wanted so finish that */
#ifdef USE_WAITQUEUE_FOR_COLLISIONS
				if(atomic_read(&tagged_page[pgid]->page_valid) != 1)
					wait_event_interruptible(tagged_page[pgid]->waiters, (atomic_read(&tagged_page[pgid]->page_valid) == 1));
#else
				while(atomic_read(&tagged_page[pgid]->page_valid) != 1)
					cpu_relax();
#endif

#ifdef USE_PROC_SYS_VM_COUNTERS
				atomic_inc(&mm_lf_stat_pgfaults_minor);
#endif
				goto pgfault_done;
			}
#if PAGES_PER_PGFAULT > 1
			else{ // if(pgid != 0)
				/* break it here! The remaining pages already exist in the buffer */
				/* XXX here the tagged_page[pgid] is locked! We have to unlock it! */
				unlock_tp(tagged_page[pgid]);
				num_pages = pgid;
				break;
			} // if(pgid == 0)
#endif
		} // if(tagged_page[pgid] == NULL)
	} // for(pgid = 0; pgid < num_pages; pgid++)
	
	/* XXX We don't have to wait here as all pages are new in buffer */

	// FIXME the following does not work for files.
	for(pgid = 0; pgid < num_pages; pgid++)
		dmap_set_page_priority(pvd, tagged_page[pgid], page_offset[pgid]);

	for(pgid = 0; pgid < num_pages; pgid++)
		DMAP_BGON(tagged_page[pgid]->page->index != page_offset[pgid]);

	for(pgid = 0; pgid < num_pages; pgid++)
		page_byte_offset[pgid] = page_offset[pgid] << PAGE_SHIFT;

	/* 
	 * Since there was not a tagged page of data for this page in the buffer then the page is new or hasn't been seen recently,
	 * the fault count is restarted at the last known value in the buffer hash table. 
	 */
	for(pgid = 0; pgid < num_pages; pgid++)
		insert_page_fifo_buffer_t(bank, tagged_page[pgid], false);

	/* Once the page lock is released, see if there was valid data for this page.  If not read if from the device. */
	dmap_do_page_io(vma, pvd, tagged_page, page_byte_offset, num_pages);

	for(pgid = 1; pgid < num_pages; pgid++)
		unlock_tp(tagged_page[pgid]);

pgfault_done:
	dmap_add_or_update_rmap(vmf, pvd, tagged_page[0], 0);

	get_page(tagged_page[0]->page);

	*pagep = tagged_page[0]->page;

	return VM_FAULT_LOCKED;
}

#ifdef USE_PAGE_FAULT_FASTPATH
static void add_mm_counter_fast(struct mm_struct *mm, int member, int val)
{
	struct task_struct *task = current;
	if(likely(task->mm == mm))
		task->rss_stat.count[member] += val;
	else
		add_mm_counter(mm, member, val);
}
#endif

/*
 * The fault method: the entry point to the file
 */

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,9,0) && LINUX_VERSION_CODE < KERNEL_VERSION(4,10,0)
int perma_vma_fault(struct vm_area_struct *vma, struct vm_fault *vmf)
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(4,14,0) && LINUX_VERSION_CODE < KERNEL_VERSION(4,15,0)
int perma_vma_fault(struct vm_fault *vmf)
#endif
{
	int ret = 0;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,14,0) && LINUX_VERSION_CODE < KERNEL_VERSION(4,15,0)
	struct vm_area_struct *vma = vmf->vma;
#endif

	//ktime_t t1, t2;
	DMAP_BGON((struct fastmap_info *)vma->vm_private_data == NULL);
	DMAP_BGON(((struct fastmap_info *)vma-vm_private_data)->is_fastmap == false);

	/* vmf->pgoff includes the base value of vma->vm_pgoff, which is how far the vma is into the address region.
	 * Therefore the vma->vm_pgoff needs to be subtracted off before computing the address */
	//t1 = ktime_get();

#ifdef USE_PROC_SYS_VM_COUNTERS
  atomic_inc(&mm_lf_stat_pgfaults);
#endif

	ret = perma_getpage(vma, &vmf->page, vmf);
	if(ret != VM_FAULT_LOCKED)
		DMAP_BGON(1);

	/* This is defined in https://elixir.bootlin.com/linux/v4.14.68/source/include/linux/page-flags.h#L76 */
	//DMAP_BGON(!PageLocked(vmf->page));

#ifdef USE_PAGE_FAULT_FASTPATH
	if(!pmd_none(*vmf->pmd)){
		struct vm_area_struct *vma = vmf->vma;
		bool write = vmf->flags & FAULT_FLAG_WRITE;
		pte_t entry;
		//spinlock_t *ptl;
		pte_t *pte;
		unsigned long cas_ret;

		//ptl = pte_lockptr(vma->vm_mm, vmf->pmd);
		pte = pte_offset_map(vmf->pmd, vmf->address);
		//vmf->ptl = ptl;
		//spin_lock(ptl);
		//vmf->pte = pte;

#if 1
		if(unlikely(!pte_none(*pte))){ // maybe we have to retry or just skip to slow path
#ifdef USE_PROC_SYS_VM_COUNTERS
			atomic_inc(&mm_lf_pgfault_fast_failed_pte);
#endif
			goto page_fault_slowpath;
		}
#else
    if(unlikely(!pte_none(*pte))){ // maybe we have to retry or just skip to slow path
      if(pte_pfn(*pte) == page_to_pfn(vmf->page)){
        if(write && pte_write(*pte) == 0){ // there is already a mapping that is not writable
          *pte = pte_mkwrite(*pte);
          *pte = maybe_mkwrite(pte_mkdirty(*pte), vma);
          return VM_FAULT_NOPAGE;
        }
      }

#ifdef USE_PROC_SYS_VM_COUNTERS
      atomic_inc(&mm_lf_pgfault_fast_failed_pte);
#endif
      goto page_fault_slowpath;
    }
#endif

		entry = mk_pte(vmf->page, vma->vm_page_prot);
		if (write)
			entry = maybe_mkwrite(pte_mkdirty(entry), vma);

		add_mm_counter_fast(vma->vm_mm, mm_counter_file(vmf->page), 1);

		// page_add_file_rmap(page, false);   ***** BEGIN ***** XXX
		//lock_page_memcg(vmf->page);
		DMAP_BGON(PageTransCompound(vmf->page) && page_mapping(vmf->page));
		atomic_inc_and_test(&vmf->page->_mapcount);
		//unlock_page_memcg(vmf->page);
		//  ***** END ***** XXX

		//set_pte_at(vma->vm_mm, vmf->address, vmf->pte, entry);
		// -OR-
		//*pte = entry;

		cas_ret = cmpxchg((unsigned long *)pte, 0, entry.pte);
		if(cas_ret != 0){ /* failed! we have to redo */
			atomic_dec_and_test(&vmf->page->_mapcount);
			add_mm_counter_fast(vma->vm_mm, mm_counter_file(vmf->page), -1);
#ifdef USE_PROC_SYS_VM_COUNTERS
			atomic_inc(&mm_lf_pgfault_fast_failed_cas);
#endif
			goto page_fault_slowpath;
		}

		//if(vmf->pte)
		//	spin_unlock(vmf->ptl);
		
		if(PageLocked(vmf->page))
	  	unlock_page(vmf->page);
			//clear_bit(PG_locked, &vmf->page->flags);

#ifdef USE_PROC_SYS_VM_COUNTERS
		atomic_inc(&mm_lf_pgfault_fast);
#endif
		
		return VM_FAULT_NOPAGE;
	}

#ifdef USE_PROC_SYS_VM_COUNTERS
	atomic_inc(&mm_lf_pgfault_slow);
#endif
page_fault_slowpath:
#endif
	
	//t2 = ktime_get();
	//atomic64_add(ktime_to_us(ktime_sub(t2, t1)), &ms_in_get_page);

	return ret;
}

/*
 * open and close: just keep track of how many times the device is
 * mapped, to avoid releasing it.
 * Open is also called when a vma is split.
 * If a close function pointer is registered, then a vma is marked as non-mergable and so once a vma is split, it cannot later be merged
 */
void perma_vma_open(struct vm_area_struct *vma)
{
	struct pr_vma_entry *pve;
	struct pr_vma_data *pvd;
	struct fastmap_info *fmap_info;

	fmap_info = (struct fastmap_info *)vma->vm_private_data;
	DMAP_BGON(fmap_info == NULL);

	pvd = fmap_info->pvd;
	DMAP_BGON(pvd == NULL);
	DMAP_BGON((pvd->magic1 != PVD_MAGIC_1) || (pvd->magic2 != PVD_MAGIC_2));
	DMAP_BGON(pvd->is_valid == false);
	
	//printk(KERN_ERR "[%s:%s:%d][%s][%p]\n", __FILE__, __func__, __LINE__, vma->vm_mm->owner->comm, vma);
	
	if(fmap_info->is_fastmap){
		DMAP_BGON(fmap_info->pvd == NULL);
		DMAP_BGON(fmap_info->pvd->magic1 != PVD_MAGIC_1);
		DMAP_BGON(fmap_info->pvd->magic2 != PVD_MAGIC_2);
		DMAP_BGON(fmap_info->pvd->is_valid == false);
		DMAP_BGON(fmap_info->pve == NULL);
		kref_get(&fmap_info->pve->refcount);
	}else{
		pve = kmem_cache_alloc(pve_alloc_cache, GFP_ATOMIC);
		DMAP_BGON(pve == NULL);

		pve->vm_start = vma->vm_start;
		pve->vm_end = vma->vm_end;
		fmap_info->is_fastmap = true;
		fmap_info->pvd = pvd;
		fmap_info->pve = pve;
	}

	atomic64_inc(&pvd->vma_count);
	atomic64_inc(&pvd->cnt);
	check_file_size(pvd);
}

static struct pr_vma_rmap *pop_one_pvr(struct pr_vma_entry *pve)
{
	int cpu;
	struct pr_vma_rmap *pvr;

	for(cpu = 0; cpu < num_online_cpus(); cpu++){
		spin_lock(&pve->pcpu_mapped_lock[cpu]);

		pvr = list_first_entry_or_null(&pve->pcpu_mapped[cpu], struct pr_vma_rmap, vma_mapped);
		if(pvr != NULL){
			list_del_init(&pvr->vma_mapped);
			spin_unlock(&pve->pcpu_mapped_lock[cpu]);
			return pvr;
		}

		spin_unlock(&pve->pcpu_mapped_lock[cpu]);
	}

	return NULL;
}

static void __pve_release(struct pr_vma_entry *pve)
{
	struct pr_vma_data *pvd;
	struct pr_vma_rmap *pvr;
	unsigned long start = 0, end = UINT_MAX;
	struct vm_area_struct *vma = NULL;
	struct page_vma_mapped_walk pvmw;

	DMAP_BGON(pve == NULL);

	while((pvr = pop_one_pvr(pve)) != NULL){
		pvr_mk_invalid(pvr);

		if(vma == NULL)
			vma = pvr_get_vma(pvr);

    memset(&pvmw, 0, sizeof(struct page_vma_mapped_walk));
    pvmw.page = pvr_get_owner(pvr)->page;
    pvmw.vma = pvr_get_vma(pvr);
    pvmw.address = pvr_get_vaddr(pvr);
    pvmw.flags = PVMW_SYNC;

    pvmw.pte = NULL;
    pvmw.ptl = NULL;

    DMAP_BGON(pvmw.vma->vm_mm == NULL);

    /* If the pte returned is valid, the lock will also be held. */
    while(page_vma_mapped_walk(&pvmw)){
      if(pvmw.pte){
        pte_t entry;
        pte_t *pte = pvmw.pte;

				entry = ptep_get_and_clear(pvmw.vma->vm_mm, pvmw.address, pte);
				if(pte_dirty(entry))
					SetPageDirty(pvr_get_owner(pvr)->page);

				/* Update high watermark before we lower rss */
				update_hiwater_rss(pvmw.vma->vm_mm);
				dec_mm_counter(pvmw.vma->vm_mm, MM_FILEPAGES);

				if(pvmw.address > start)
					start = pvmw.address;

				if(pvmw.address < end)
					end = pvmw.address;
			}
		
			pvd = pvr_get_owner(pvr)->pvd;
			DMAP_BGON(pvd == NULL);
		}
	}
	
	if(vma != NULL)
		flush_tlb_mm_range(vma->vm_mm, start, end + PAGE_SIZE, VM_NONE);
}

static void pve_release(struct kref *kref)
{
	struct pr_vma_entry *pve = container_of(kref, struct pr_vma_entry, refcount);
	__pve_release(pve);
}

void perma_vma_close(struct vm_area_struct *vma)
{
	//struct pr_vma_data *pvd;
	struct pr_vma_entry *pve;
#ifdef ENABLE_IO_STATS
	ktime_t t1, t2;

	t1 = ktime_get();
	atomic64_inc(&buf_data->num_munmap);
#endif

	//printk(KERN_ERR "[%s:%s:%d][%s][%p]\n", __FILE__, __func__, __LINE__, vma->vm_mm->owner->comm, vma);

	DMAP_BGON(((struct fastmap_info *)vma->vm_private_data)->is_fastmap == false);
	//DMAP_BGON(vma->pvd == NULL);
	DMAP_BGON(((struct fastmap_info *)vma->vm_private_data)->pve == NULL);

	//pvd = (struct pr_vma_data *)vma->pvd;
	//DMAP_BGON(pvd == NULL);

	//if(pvd->is_valid == false)
	//	return;

	//DMAP_BGON((pvd->magic1 != PVD_MAGIC_1) || (pvd->magic2 != PVD_MAGIC_2));

	pve = ((struct fastmap_info *)vma->vm_private_data)->pve;
	DMAP_BGON(pve == NULL);

	if(kref_put(&pve->refcount, pve_release) == 1){ // pve_release called!
		 kmem_cache_free(pve_alloc_cache, pve);
		 ((struct fastmap_info *)vma->vm_private_data)->is_fastmap = false;
	}

	//atomic64_dec(&pvd->cnt);
	//atomic64_dec(&pvd->vma_count);

#ifdef ENABLE_IO_STATS
	t2 = ktime_get();
	atomic64_add(ktime_to_ms(ktime_sub(t2, t1)), &buf_data->ms_in_munmap);
#endif

	// reset priorities
	//if(pvd->type == D_BLKD)
	//	memset(pvd->meta.rdd->meta, 0, (get_capacity(pvd->meta.rdd->binding->bd_disk) >> 3) * sizeof(struct device_meta));
	
	//printk(KERN_ERR "[%s:%s:%d][%s][%p]\n", __FILE__, __func__, __LINE__, vma->vm_mm->owner->comm, vma);
}

void print_status_dimmap_buffer()
{
	print_status(buf_data);
}

void reset_stats_dimmap_buffer(){}

int perma_page_mkwrite(struct vm_fault *vmf)
{
	unsigned long page_offset;
	struct tagged_page *tagged_page;
	struct pr_vma_data *pvd;
	struct fastmap_info *fmap_info;
	struct vm_area_struct *vma = vmf->vma;
	int tl = -1;

#ifdef USE_PROC_SYS_VM_COUNTERS
  atomic_inc(&mm_lf_stat_mkwrites);
#endif

	fmap_info = (struct fastmap_info *)vma->vm_private_data;
	DMAP_BGON(fmap_info == NULL);

	pvd = fmap_info->pvd;
	DMAP_BGON(pvd == NULL);

	page_offset = vmf->pgoff;
	pvd->is_readonly = false;

	while(1){
		tagged_page = (struct tagged_page *)vmf->page->private;

		tl = trylock_tp(tagged_page, page_offset);
		if(tl == -2) /* wrong index */
			return VM_FAULT_NOPAGE;
		else if(tl == -3) /* not in use */
			return VM_FAULT_NOPAGE;
		else if(tl != 0){ // failed to lock the page
			wait_on_page_bit(tagged_page->page, PG_locked);
			continue;
		}
		// tl == 0 -> locked the page
		//else{
		//	BUG();
		//}

		DMAP_BGON(atomic_read(&tagged_page->page_valid) == 0);
		DMAP_BGON(atomic_read(&tagged_page->in_use) == 0);
		DMAP_BGON((pvd->dev_id != tagged_page->pvd->dev_id) || (pvd->ino != tagged_page->pvd->ino));
		dmap_do_tagged_page_writable(vma, pvd, tagged_page);

		return VM_FAULT_LOCKED;
	}
}

#ifdef USE_MAP_PAGES
static void dmap_map_pages(struct vm_fault *vmf, struct radix_tree_root *rdx_root, pgoff_t start_pgoff, pgoff_t end_pgoff)
{
	struct pr_vma_data *pvd = ((struct fastmap_info *)vmf->vma->vm_private_data)->pvd;
	struct radix_tree_iter iter;
	void **slot;
	struct file *file = wrapfs_lower_file(vmf->vma->vm_file); //pvd->bk.filp;
	struct address_space *mapping = file->f_mapping;
	pgoff_t last_pgoff = start_pgoff; 
	unsigned long max_idx;
	struct tagged_page *tp;

	DMAP_BGON(pvd == NULL);

#ifdef USE_PROC_SYS_VM_COUNTERS
  atomic_inc(&mm_lf_stat_map_pages);
	atomic_inc(&mm_lf_stat_call_maps);
#endif /* USE_PROC_SYS_VM_COUNTERS */

	rcu_read_lock();
	radix_tree_for_each_slot(slot, rdx_root, &iter, start_pgoff)
	{
		if(iter.index > end_pgoff)
			break;
repeat:
		tp = radix_tree_deref_slot(slot);
		if(unlikely(!tp))
			goto next;
		if(radix_tree_exception(tp)){
			if(radix_tree_deref_retry(tp)){
				slot = radix_tree_iter_retry(&iter);
				continue;
			}
			goto next;
		}

		if(!page_cache_get_speculative(tp->page))
			goto repeat;

		/* Has the page moved? */
		if(unlikely(tp != *slot)){
			put_page(tp->page);
			goto repeat;
		}

		if(atomic_read(&tp->page_valid) == 0) 
			goto skip;

		if(trylock_tp(tp, ULONG_MAX) != 0)
			goto skip;

		DMAP_BGON(atomic_read(&tagged_page->in_use) == 0);

		if(atomic_read(&tp->page_valid) == 0)
			goto unlock;

		max_idx = DIV_ROUND_UP(i_size_read(mapping->host), PAGE_SIZE);
		if(tp->page->index >= max_idx)
			goto unlock;

		if(file->f_ra.mmap_miss > 0)
			file->f_ra.mmap_miss--;

		vmf->address += (tp->page->index - last_pgoff) << PAGE_SHIFT;
		if(vmf->pte)
			vmf->pte += tp->page->index - last_pgoff;
		last_pgoff = tp->page->index;

#ifdef USE_MAP_PAGES_FASTPATH
    if(!pmd_none(*vmf->pmd)){
      struct vm_area_struct *vma = vmf->vma;
      pte_t entry;
      pte_t *pte;
      unsigned long cas_ret;

      pte = pte_offset_map(vmf->pmd, vmf->address);
      if(unlikely(!pte_none(*pte))) // maybe we have to retry or just skip to slow path
        goto map_pages_slowpath;

      entry = mk_pte(tp->page, vma->vm_page_prot);
      add_mm_counter_fast(vma->vm_mm, mm_counter_file(tp->page), 1);
      atomic_inc_and_test(&tp->page->_mapcount);

      cas_ret = cmpxchg((unsigned long *)pte, 0, entry.pte);
      if(cas_ret != 0){ /* failed! we have to redo */
        atomic_dec_and_test(&tp->page->_mapcount);
        add_mm_counter_fast(vma->vm_mm, mm_counter_file(tp->page), -1);
        goto map_pages_slowpath;
      }
    }else{
map_pages_slowpath:
#endif /* USE_MAP_PAGES_FASTPATH */
			if(alloc_set_pte(vmf, NULL, tp->page)){
#ifdef USE_PROC_SYS_VM_COUNTERS
				atomic_inc(&mm_lf_stat_fail_maps);
#endif
				goto unlock;
			}
#ifdef USE_PROC_SYS_VM_COUNTERS
			atomic_inc(&mm_lf_stat_succ_maps);
#endif
#ifdef USE_MAP_PAGES_FASTPATH
		}
#endif /* USE_MAP_PAGES_FASTPATH */

		dmap_add_or_update_rmap(vmf, pvd, tp, 0);

		unlock_tp(tp);
		goto next;
unlock:
		unlock_tp(tp);
skip:
		put_page(tp->page); 
next:
		/* Huge page is mapped? No need to proceed. */
		if(pmd_trans_huge(*vmf->pmd))
			break;
		if(iter.index == end_pgoff)
			break;
	}
	rcu_read_unlock();
}

#ifdef USE_PERCPU_RADIXTREE
static void perma_map_pages(struct vm_fault *vmf, pgoff_t start_pgoff, pgoff_t end_pgoff)
{
	unsigned long addr = vmf->address;
	struct pr_vma_data *pvd = ((struct fastmap_info *)vmf->vma->vm_private_data)->pvd;
	int cpu;
	bool has_pte = (vmf->pte == NULL)?(false):(true);

	for(cpu = 0; cpu < num_online_cpus(); cpu++){
		vmf->address = addr;
		if(has_pte){
			vmf->pte = pte_offset_map(vmf->pmd, vmf->address);
			vmf->orig_pte = *vmf->pte;
		}
		dmap_map_pages(vmf, &(pvd->rdx[cpu]), start_pgoff, end_pgoff);
		if(vmf->pte != NULL)
			has_pte = true;
	}
}
#else /* USE_PERCPU_RADIXTREE */
static void perma_map_pages(struct vm_fault *vmf, pgoff_t start_pgoff, pgoff_t end_pgoff)
{
	struct pr_vma_data *pvd = (struct pr_vma_data *)vmf->vma->pvd;

	dmap_map_pages(vmf, &(pvd->rdx), start_pgoff, end_pgoff);
}
#endif /* USE_PERCPU_RADIXTREE */

#endif /* USE_MAP_PAGES */


static const char *perma_name(struct vm_area_struct *vma) 
{ 
	return "[dmap]"; 
}

static int perma_split(struct vm_area_struct *area, unsigned long addr)
{
	bool fmap;
	struct pr_vma_entry *pve;
	struct pr_vma_data *pvd;

	struct fastmap_info *fmap_info = (struct fastmap_info *)area->vm_private_data;
	DMAP_BGON(fmap_info == NULL);

	fmap = fmap_info->is_fastmap;
	pve = fmap_info->pve;
	pvd = fmap_info->pvd;

	printk(KERN_ERR "[%s:%s:%d]is[%d]pvd[%p]pve[%p]addr[%lu]\n", __FILE__, __func__, __LINE__, fmap, pvd, pve, addr);
	return 0; 
}

static int perma_mremap(struct vm_area_struct *area)
{
	struct pr_vma_entry *old_pve = ((struct fastmap_info *)area->vm_private_data)->pve;
	struct pr_vma_entry *new_pve = NULL;
	struct list_head *node;
	unsigned long diff;
	bool new_is_higher = false;
	int cpu, my_cpu;

	DMAP_BGON(old_pve == NULL);
	DMAP_BGON(area->vm_start == old_pve->vm_start);

	//printk(KERN_ERR "[%s:%s:%d]\n", __FILE__, __func__, __LINE__);

	if(area->vm_start > old_pve->vm_start)
		new_is_higher = true;

	diff = (new_is_higher)?(area->vm_start - old_pve->vm_start):(old_pve->vm_start - area->vm_start);

	new_pve = kmem_cache_alloc(pve_alloc_cache, GFP_ATOMIC);
	DMAP_BGON(new_pve == NULL);

	new_pve->vm_start = area->vm_start;
	new_pve->vm_end = area->vm_end;

	// we don't lock new_pve as we only change it
	for(cpu = 0; cpu < num_online_cpus(); cpu++){
		spin_lock(&old_pve->pcpu_mapped_lock[cpu]);

		list_for_each(node, &old_pve->pcpu_mapped[cpu]){
			int i;
			struct pr_vma_rmap *old_pvr = rb_entry(node, struct pr_vma_rmap, vma_mapped);
			struct tagged_page *tp = pvr_get_owner(old_pvr);
			struct pr_vma_rmap *new_pvr = NULL; 
			
			spin_lock(&tp->rmap_lock);
			for(i = 0; i < MAX_RMAPS_PER_PAGE; i++){
				if(pvr_is_valid(&tp->rmap[i]) == false){
					pvr_mk_valid(&tp->rmap[i]);
					pvr_set_idx(&tp->rmap[i], i);
					new_pvr = &(tp->rmap[i]);
					break;
				}
			}
			spin_unlock(&tp->rmap_lock);

			DMAP_BGON(!new_pvr);

			pvr_mk_valid(new_pvr);
			pvr_set_vaddr(new_pvr, (new_is_higher)?(pvr_get_vaddr(old_pvr) + diff):(pvr_get_vaddr(old_pvr) - diff));
			pvr_set_vma(new_pvr, area);
			pvr_set_cpu(new_pvr, 0);

			my_cpu = smp_processor_id();
			spin_lock(&new_pve->pcpu_mapped_lock[my_cpu]);
			list_add_tail(&new_pvr->vma_mapped, &new_pve->pcpu_mapped[my_cpu]);
			pvr_set_cpu(new_pvr, my_cpu);
			spin_unlock(&new_pve->pcpu_mapped_lock[my_cpu]);
		}

		spin_unlock(&old_pve->pcpu_mapped_lock[cpu]);
	}
	
	// We ignore old_pve. Someone else will close it!
	((struct fastmap_info *)area->vm_private_data)->pve = new_pve;

	return 0;
}

#ifdef USE_HUGEPAGES
static pmd_t maybe_pmd_mkwrite(pmd_t pmd, struct vm_area_struct *vma)
{
	if (likely(vma->vm_flags & VM_WRITE))
		pmd = pmd_mkwrite(pmd);
	return pmd;
}

/*
 * int dev_dax_huge_fault(struct vm_fault *vmf,
 * 		enum page_entry_size pe_size)
 */
int perma_huge_fault(struct vm_fault *vmf, enum page_entry_size pe_size)
{
	struct page *pg = NULL;
	struct mm_struct *mm = vmf->vma->vm_mm;
	pmd_t entry;
	spinlock_t *ptl;
	bool write = vmf->flags & FAULT_FLAG_WRITE;
	unsigned long pmd_addr = vmf->address & PMD_MASK;
	unsigned long haddr = vmf->address & HPAGE_PMD_MASK;
	pgprot_t prot = vmf->vma->vm_page_prot;
	//pfn_t pfn;
	pgoff_t pgoff;

	/* not implemented yet */
	if(pe_size == PE_SIZE_PUD) /* 1GB */
		return VM_FAULT_FALLBACK;

	if(pe_size == PE_SIZE_PMD){ /* 2MB */
		printk(KERN_ERR "perma_huge_fault - PE_SIZE_PMD - 2MB\n");

		spin_lock(&hugepages_lock);
		printk(KERN_ERR "hugepages_idx = %lu\n", hugepages_idx);
		pg = hugepages[hugepages_idx];
		hugepages_idx++;
		spin_unlock(&hugepages_lock);
		DMAP_BGON(pg == NULL);

		//pfn = page_to_pfn_t(pg);
		pgoff = linear_page_index(vmf->vma, pmd_addr);

		ptl = pmd_lock(mm, vmf->pmd);
		if(unlikely(!pmd_none(*vmf->pmd)))
			goto out;

		//entry = pmd_mkhuge(pfn_t_pmd(pfn, prot));
		entry = mk_huge_pmd(pg, prot);

		//if(pfn_t_devmap(pfn))
		//	entry = pmd_mkdevmap(entry);

		if(write){
			entry = pmd_mkyoung(pmd_mkdirty(entry));
			entry = maybe_pmd_mkwrite(entry, vmf->vma);
		}

		add_mm_counter(vmf->vma->vm_mm, MM_FILEPAGES, HPAGE_PMD_NR);
		//page_add_file_rmap(pg, true);

		set_pmd_at(mm, haddr, vmf->pmd, entry);

		update_mmu_cache_pmd(vmf->vma, haddr, vmf->pmd);
out:
		spin_unlock(ptl);	
		return VM_FAULT_NOPAGE;
	}

	DMAP_BGON(pe_size == PE_SIZE_PTE); /* 4KB */
	return -1;
}
#endif

struct vm_operations_struct perma_vm_ops_file = {
	.open = perma_vma_open,
	.close = perma_vma_close,
	.split = perma_split,
	.mremap = perma_mremap,
	.fault = perma_vma_fault,
#ifdef USE_HUGEPAGES
	.huge_fault = perma_huge_fault,
#endif
#ifdef USE_MAP_PAGES
	.map_pages = perma_map_pages,
#endif
	.page_mkwrite = perma_page_mkwrite,
	//.pfn_mkwrite
	//.access
	.name = perma_name,
	//.set_policy
	//.get_policy
	//.find_special_page
};

struct vm_operations_struct perma_vm_ops_blockdev = {
	.open = perma_vma_open,
	.close = perma_vma_close,
	.split = perma_split,
	.mremap = perma_mremap,
	.fault = perma_vma_fault,
#ifdef USE_HUGEPAGES
	.huge_fault = perma_huge_fault,
#endif
	.page_mkwrite = perma_page_mkwrite,
	.name = perma_name,
};

int dmap_mmap_blockdev(struct file *filp, struct vm_area_struct *vma)
{
	vma->vm_ops = &perma_vm_ops_blockdev;

	//printk(KERN_ERR "[%s:%s:%d][%s][%p]\n", __FILE__, __func__, __LINE__, vma->vm_mm->owner->comm, vma);

	if (vma->vm_pgoff != 0)
		printk(KERN_ERR "dmap_mmap_blockdev() -> vma->vm_pgoff = %lu\n", vma->vm_pgoff);

	/* Disable any system potential readahead for these mappings */
	vma->vm_flags = (vma->vm_flags & ~VM_SEQ_READ) | VM_RAND_READ;
	vma->vm_private_data = (struct fastmap_info *)filp->private_data;
	DMAP_BGON(vma->vm_private_data == NULL);

	/* Allow the fault handler to insert pages via vm_insert_page: requires the VM_MIXEDMAP flag */
	vma->vm_flags |= VM_MIXEDMAP;

	//printk(KERN_ERR "We have mmaped a region that has a file %p and a vma->vm_file %p (offset=%lx) and address %lx to %lx with flags %lx : vma=%p mm=%p\n",
	//										filp, vma->vm_file, vma->vm_pgoff, vma->vm_start, vma->vm_end, vma->vm_flags, vma, vma->vm_mm);
	perma_vma_open(vma);

	return 0;
}

int dmap_mmap_file(struct file *file, struct vm_area_struct *vma)
{
	int err = 0;
	bool willwrite;
	struct file *lower_file;
	const struct vm_operations_struct *saved_vm_ops = NULL;
	struct pr_vma_data *pvd;

	/* this might be deferred to mmap's writepage */
	willwrite = ((vma->vm_flags | VM_SHARED | VM_WRITE) == vma->vm_flags);

	/*  
	 * File systems which do not implement ->writepage may use
	 * generic_file_readonly_mmap as their ->mmap op.  If you call
	 * generic_file_readonly_mmap with VM_WRITE, you'd get an -EINVAL.
	 * But we cannot call the lower ->mmap op, so we can't tell that
	 * writeable mappings won't work.  Therefore, our only choice is to
	 * check if the lower file system supports the ->writepage, and if
	 * not, return EINVAL (the same error that
	 * generic_file_readonly_mmap returns in that case).
	 */
	lower_file = wrapfs_lower_file(file);
	if (willwrite && !lower_file->f_mapping->a_ops->writepage)
	{
		err = -EINVAL;
		printk(KERN_ERR "wrapfs: lower file system does not support writeable mmap\n");
		goto out;
	}

	//if (vma->vm_pgoff != 0)
	//	printk(KERN_ERR "dmap_mmap_file() -> vma->vm_pgoff = %lu\n", vma->vm_pgoff);

	//printk(KERN_ERR "file size = %lu, mmap size = %lu\n", lower_file->f_inode->i_size,  vma->vm_end - vma->vm_start);

	atomic64_set(&((struct wrapfs_file_info *)file->private_data)->fmap_info->pvd->mmaped, 1);

	//err = lower_file->f_op->mmap(file, vma); // for ext4 it does not allocate anything
	//if(err == 0)
	//	lower_page_mkwrite = vma->vm_ops->page_mkwrite;
	//err = 0;

	/*  
	 * find and save lower vm_ops.
	 *
	 * XXX: the VFS should have a cleaner way of finding the lower vm_ops
	 */
	if (!WRAPFS_F(file)->lower_vm_ops)
		saved_vm_ops = vma->vm_ops; /* save: came from lower ->mmap */

	/*  
	 * Next 3 lines are all I need from generic_file_mmap.  I definitely
	 * don't want its test for ->readpage which returns -ENOEXEC.
	 */
	file_accessed(lower_file);
	file_accessed(file);
	vma->vm_ops = &perma_vm_ops_file;

	/* Disable any system potential readahead for these mappings */
	vma->vm_flags = (vma->vm_flags & ~VM_SEQ_READ) | VM_RAND_READ;
	DMAP_BGON(vma->vm_private_data != NULL);
	vma->vm_private_data = ((struct wrapfs_file_info *)file->private_data)->fmap_info;

	pvd = ((struct wrapfs_file_info *)file->private_data)->fmap_info->pvd;
	DMAP_BGON(pvd == NULL);
	pvd->is_mmap = true;
	check_file_size(pvd);

	/* Allow the fault handler to insert pages via vm_insert_page: requires the VM_MIXEDMAP flag */
	vma->vm_flags |= VM_MIXEDMAP;

	//printk(KERN_ERR "We have mmaped a region that has a file %p and a vma->vm_file %p (offset=%lx) and address %lx to %lx with flags %lx : vma=%p mm=%p\n",
	//										file, vma->vm_file, vma->vm_pgoff, vma->vm_start, vma->vm_end, vma->vm_flags, vma, vma->vm_mm);
	//printk(KERN_ERR "[%d] mmaped a file with ino %lu in device %d:%d\n", current->pid, pvd->ino, MAJOR(pvd->dev_id), MINOR(pvd->dev_id));
	perma_vma_open(vma);

	file->f_mapping->a_ops = &wrapfs_aops; /* set our aops */
	if (!WRAPFS_F(file)->lower_vm_ops) /* save for our ->fault */
		WRAPFS_F(file)->lower_vm_ops = saved_vm_ops;
out:
	return err;
}
