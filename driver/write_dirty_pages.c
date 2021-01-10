#include <linux/version.h>
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/uio.h>
#include <linux/file.h>

#include "mmap_fault_handler.h"
#include "mmap_buffer_interface.h"
#include "mmap_buffer_hash_table.h"
#include "mmap_buffer_rbtree.h"
#include "shared_defines.h"
#include "dmap.h"

#define LCHILD(x) 2 * x + 1
#define RCHILD(x) 2 * x + 2
#define PARENT(x) (x - 1) / 2

#define MAX_CPUS (64)

struct submit_bio_ret {
	struct completion event;
	int error;
};

struct heap_node {
	int cpu;
	pgoff_t offset;
	struct tagged_page *p;
};

struct min_heap {
	struct heap_node heap_array[MAX_CPUS];
	int heap_size;
};

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,9,0) && LINUX_VERSION_CODE < KERNEL_VERSION(4,10,0)
static void submit_bio_wait_endio(struct bio *bio)
{
	struct submit_bio_ret *ret = bio->bi_private;

	ret->error = bio->bi_error;
	complete(&ret->event);
}
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(4,14,0) && LINUX_VERSION_CODE < KERNEL_VERSION(4,15,0)
static void submit_bio_wait_endio(struct bio *bio)
{
	struct submit_bio_ret *ret = bio->bi_private;

	ret->error = blk_status_to_errno(bio->bi_status);
	complete(&ret->event);
}
#endif

static struct min_heap *min_heap_create(void)
{
	struct min_heap *mh;

	mh = page_address(alloc_page(GFP_KERNEL));

	memset(mh->heap_array, 0, MAX_CPUS * sizeof(struct heap_node));
	mh->heap_size = 0;

	return mh;
}

static void min_heap_destroy(struct min_heap *mh)
{
	free_page((unsigned long)mh);
}

static bool min_heap_is_empty(struct min_heap *mh)
{
	return (mh->heap_size == 0)?(true):(false);
}

static void min_heap_insert(struct min_heap *mh, int cpu, pgoff_t offset, struct tagged_page *p)
{
	int i = mh->heap_size++;
	DMAP_BGON(i >= MAX_CPUS);

	while(i && offset < mh->heap_array[PARENT(i)].offset){
		mh->heap_array[i] = mh->heap_array[PARENT(i)];
		i = PARENT(i);
	}

	mh->heap_array[i].cpu = cpu;
	mh->heap_array[i].offset = offset;
	mh->heap_array[i].p = p;
}

static void min_heap_heapify(struct min_heap *mh, int i) 
{
	int smallest = (LCHILD(i) < mh->heap_size && mh->heap_array[LCHILD(i)].offset < mh->heap_array[i].offset)?(LCHILD(i)):(i);

	if(RCHILD(i) < mh->heap_size && mh->heap_array[RCHILD(i)].offset < mh->heap_array[smallest].offset)
		smallest = RCHILD(i);

	if(smallest != i){
		swap(mh->heap_array[i], mh->heap_array[smallest]);
		min_heap_heapify(mh, smallest);
	}
}

/* returns the element in @arg ret */
static void min_heap_delete_min(struct min_heap *mh, struct heap_node *r)
{
	DMAP_BGON(mh->heap_size <= 0);
	DMAP_BGON(r == NULL);

	r->cpu = mh->heap_array[0].cpu;
	r->offset = mh->heap_array[0].offset;
	r->p = mh->heap_array[0].p;

	mh->heap_array[0] = mh->heap_array[--mh->heap_size];
	min_heap_heapify(mh, 0);
}

void write_file_async(struct tagged_page **p, int len)
{
	struct kiocb iocb;
	struct bio_vec *bvec;
	struct iov_iter iter;
	struct tagged_page *base;
	int i, k, base_idx;
	int *merge_arr;
	unsigned long total_size, num_pages;
	unsigned order;
	struct page *pg;
	unsigned char *base_address;
  struct inode *inode;
	//ssize_t (*direct_IO)(struct kiocb *, struct iov_iter * iter);
	ssize_t (*write_iter) (struct kiocb *, struct iov_iter *);
  ssize_t retval;
	struct file *lower_file;
	int overwrite = 0;

	if(unlikely(len == 0))
		return;

	total_size = len * (sizeof(int) + sizeof(struct bio_vec));
	num_pages = (total_size / PAGE_SIZE) + 1;
	if(!is_power_of_2(num_pages))
		num_pages = __roundup_pow_of_two(num_pages);
	order = ilog2(num_pages);
	
	pg = alloc_pages_current(GFP_KERNEL, order);
	DMAP_BGON(pg == NULL);

	base_address = (unsigned char *)page_address(pg);
	merge_arr = (int *)base_address;
	bvec = (struct bio_vec *)(base_address + (len * sizeof(int)));

	for(i = 0; i < len; i++)
		merge_arr[i] = 1; // initialize and merge_arr in order to remove one for loop

	// compute merges
	base_idx = 0;
	base = p[0];
	for(i = 1; i < len; i++){
		if(base->page->index + 1 == p[i]->page->index){
			merge_arr[base_idx]++;
			merge_arr[i]--;
			if(merge_arr[base_idx] == BIO_MAX_PAGES)
				base_idx = i;
		}else{
			base_idx = i;
		}
		base = p[i];
	}

  for(i = 0; i < len; i++){
		if(merge_arr[i] == 0)
			continue;
	
		DMAP_BGON(p[i] == NULL);

		if(p[i]->pvd == NULL)
			continue;

		DMAP_BGON(p[i]->pvd->bk.filp == NULL);

		lower_file = p[i]->pvd->bk.filp;
		DMAP_BGON(lower_file == NULL);
	
		inode = file_inode(lower_file);
		//direct_IO = lower_file->f_inode->i_mapping->a_ops->direct_IO;
		write_iter = lower_file->f_op->write_iter;

		iocb.ki_filp = lower_file;
		iocb.ki_pos = p[i]->page->index << PAGE_SHIFT;
		iocb.ki_complete = NULL;
		iocb.private = &overwrite;
		iocb.ki_flags =  IOCB_DIRECT | IOCB_WRITE; // | IOCB_SYNC | IOCB_DSYNC,

		for(k = 0; k < merge_arr[i]; k++){
			bvec[k].bv_page = p[i+k]->page;
			bvec[k].bv_len = PAGE_SIZE;
			bvec[k].bv_offset = 0;
		}

		iter.type = WRITE | ITER_BVEC;
		iter.iov_offset = 0;
		iter.count = merge_arr[i] * PAGE_SIZE;
		iter.bvec = bvec;
		iter.nr_segs = merge_arr[i];

		//get_file(lower_file); /* prevent lower_file from being released */
		//inode_lock(inode);

		//retval = direct_IO(&iocb, &iter);
		retval = write_iter(&iocb, &iter);
		if(retval !=  merge_arr[i] * PAGE_SIZE){
			printk(KERN_ERR "[%s:%s:%d] write_iter(...) returns %zd\n", __FILE__, __func__, __LINE__, retval);
			DMAP_BGON(1);
		}

		//inode_unlock(inode);
		//fput(lower_file);
	}

	__free_pages(pg, order);
}

void write_bio_async(struct tagged_page **p, int len)
{
	struct submit_bio_ret *ret;
	struct bio **bio;
	struct tagged_page *base;
	int i, k, base_idx, bret;
	int *merge_arr;
	unsigned long total_size, num_pages;
	unsigned order;
	struct page *pg;
	unsigned char *base_address;

	if(unlikely(len == 0))
		return;

	total_size = len * (sizeof(int) + sizeof(struct submit_bio_ret) + sizeof(struct bio *));
	num_pages = (total_size / PAGE_SIZE) + 1;
	if(!is_power_of_2(num_pages))
		num_pages = __roundup_pow_of_two(num_pages);
	order = ilog2(num_pages);

	pg = alloc_pages_current(GFP_KERNEL, order);
	DMAP_BGON(pg == NULL);

	base_address = (unsigned char *)page_address(pg);
	//merge_arr = (int *)base_address;
	//ret = (struct submit_bio_ret *)	(base_address + (len * sizeof(int)) );
	//bio = (struct bio **) (base_address + (len * sizeof(int)) + (len * sizeof(struct submit_bio_ret)) );

	merge_arr = vmalloc((len + 1) * sizeof(int));
	ret = vmalloc((len + 1) * sizeof(struct submit_bio_ret));
	bio = vmalloc((len + 1) * sizeof(struct bio *));

	for(i = 0; i < (len + 1); i++){
		bio[i] = NULL;
		merge_arr[i] = 1; // initialize and merge_arr in order to remove one for loop
	}

	// compute merges
	base_idx = 0;
	base = p[0];
	for(i = 1; i < len; i++){
		if(base->page->index + 1 == p[i]->page->index){
			merge_arr[base_idx]++;
			merge_arr[i]--;
			if(merge_arr[base_idx] == BIO_MAX_PAGES)
				base_idx = i;
		}else{
			base_idx = i;
		}
		base = p[i];
	}

	for(i = 0; i < len; i++){
		if(merge_arr[i] == 0){
			bio[i] = NULL;
			continue;
		}

		//printk(KERN_ERR "[%s:%s:%d] merge_arr[%d] = %d\n", __FILE__, __func__, __LINE__, i, merge_arr[i]);

		bio[i] = bio_alloc(GFP_NOIO, merge_arr[i]);
		if(bio[i] == NULL){
			printk(KERN_ERR "BIO ERROR! merge_arr[i] = %d\n",  merge_arr[i]);
		}
		DMAP_BGON(bio[i] == NULL);

		DMAP_BGON(p[i]->pvd->bk.bdev == NULL);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,9,0) && LINUX_VERSION_CODE < KERNEL_VERSION(4,10,0)
		bio[i]->bi_bdev = p[i]->pvd->bk.bdev;
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(4,14,0) && LINUX_VERSION_CODE < KERNEL_VERSION(4,15,0)
		bio_set_dev(bio[i], p[i]->pvd->bk.bdev);
#endif
		bio_set_op_attrs(bio[i], REQ_OP_WRITE, 0);
		bio[i]->bi_iter.bi_sector = p[i]->page->index << 3;

		for(k = 0; k < merge_arr[i]; k++){
			bret = bio_add_page(bio[i], p[i+k]->page, PAGE_SIZE, 0);
			DMAP_BGON(bret != PAGE_SIZE);
		}

		init_completion(&(ret[i].event));
		bio[i]->bi_private = &(ret[i]);
		bio[i]->bi_end_io = submit_bio_wait_endio;
		submit_bio(bio[i]);
	}

	for(i = 0; i < len; i++){
		if(bio[i] != NULL){
			wait_for_completion_io(&(ret[i].event));
			bio_put(bio[i]);
		}
	}

	vfree(merge_arr);
	vfree(ret);
	vfree(bio);

	__free_pages(pg, order);
}

void write_dirty_pages(struct pr_vma_data *pvd, void (*__page_cleanup)(struct tagged_page *))
{
	int cpu, ncpus = num_online_cpus();
#ifndef USE_RADIX_TREE_FOR_DIRTY
	struct rb_node *node;
#endif
	struct tagged_page *p;
	struct heap_node hn;
	struct min_heap *mh;

	struct page *pg;
	struct tagged_page **pages;
	int i, num_pages, max_pages;
	bool locked;

#ifdef USE_RADIX_TREE_FOR_DIRTY
	// XXX max 128 cores
	struct radix_tree_iter *__iter = (struct radix_tree_iter *)__get_free_page(GFP_KERNEL);
	void __rcu **__slot[128] = { NULL };

	BUG_ON((128 * sizeof(struct radix_tree_iter)) > PAGE_SIZE);

	for(cpu = 0; cpu < ncpus; ++cpu)
		__slot[cpu] = radix_tree_iter_init(&__iter[cpu], 0);
#endif

	pg = alloc_page(GFP_KERNEL);
	pages = page_address(pg);
	num_pages = 0;
	max_pages = PAGE_SIZE / sizeof(struct tagged_page *); /* 512 pages */
	//max_pages = max_pages / 2; /* 256 pages */
	//max_pages = max_pages / 2; /* 128 pages */

	//printk(KERN_ERR "[%s:%s:%d]\n", __FILE__, __func__, __LINE__);
	
	mh = min_heap_create();
	
	/* first we get an element from each dirty tree and 
	 * add it to the min_heap */
	for(cpu = 0; cpu < ncpus; ++cpu)
	{
		locked = false;
		while(!locked){
#ifdef USE_RADIX_TREE_FOR_DIRTY
			rcu_read_lock();
			if(__slot[cpu] || (__slot[cpu] = radix_tree_next_chunk(&pvd->dirty_tree[cpu], &__iter[cpu], 0))){

				p = radix_tree_deref_slot(__slot[cpu]);
				if(trylock_tp(p, ULONG_MAX) != 0){ // failed to lock page
					rcu_read_unlock();
					cpu_relax();
					continue;
				}
				
				locked = true;
				DMAP_BGON(atomic_read(&p->in_use) == 0);

				min_heap_insert(mh, cpu, p->page->index, p);

				spin_lock(&pvd->tree_lock[cpu]);
				radix_tree_iter_delete(&pvd->dirty_tree[cpu], &__iter[cpu], __slot[cpu]);
				spin_unlock(&pvd->tree_lock[cpu]);

				__slot[cpu] = radix_tree_next_slot(__slot[cpu], &__iter[cpu], 0);
			}else
				 locked = true;
			rcu_read_unlock();
#else
			spin_lock(&pvd->tree_lock[cpu]);
			if(!RB_EMPTY_ROOT(&pvd->dirty_tree[cpu])){
				node = rb_first(&pvd->dirty_tree[cpu]);
				p = rb_entry(node, struct tagged_page, node);

				if(trylock_tp(p, ULONG_MAX) != 0){ // failed to lock page
					spin_unlock(&pvd->tree_lock[cpu]);
					cpu_relax();
					continue;
				}
				locked = true;
				DMAP_BGON(atomic_read(&p->in_use) == 0);

				min_heap_insert(mh, cpu, p->page->index, p);
				rb_erase(node, &pvd->dirty_tree[cpu]); /* we already have the tree lock */
			}else
				locked = true;
			spin_unlock(&pvd->tree_lock[cpu]);
#endif
		}
	}
	
	//printk(KERN_ERR "[%s:%s:%d]\n", __FILE__, __func__, __LINE__);

	while(!min_heap_is_empty(mh)){
		min_heap_delete_min(mh, &hn);

		/* add a new element from the correct dirty tree */
    locked = false;
    while(!locked){	
#ifdef USE_RADIX_TREE_FOR_DIRTY
			rcu_read_lock();
      if(__slot[hn.cpu] || (__slot[hn.cpu] = radix_tree_next_chunk(&pvd->dirty_tree[hn.cpu], &__iter[hn.cpu], 0))){

        p = radix_tree_deref_slot(__slot[hn.cpu]);
        if(trylock_tp(p, ULONG_MAX) != 0){ // failed to lock page
					rcu_read_unlock();
					cpu_relax();
          continue;
				}

        locked = true;
        DMAP_BGON(atomic_read(&p->in_use) == 0);

				min_heap_insert(mh, hn.cpu, p->page->index, p);

        spin_lock(&pvd->tree_lock[hn.cpu]);
        radix_tree_iter_delete(&pvd->dirty_tree[hn.cpu], &__iter[hn.cpu], __slot[hn.cpu]);
        spin_unlock(&pvd->tree_lock[hn.cpu]);

				__slot[hn.cpu] = radix_tree_next_slot(__slot[hn.cpu], &__iter[hn.cpu], 0);
      }else
         locked = true;
      rcu_read_unlock();
#else
			spin_lock(&pvd->tree_lock[hn.cpu]);
			if(!RB_EMPTY_ROOT(&pvd->dirty_tree[hn.cpu])){
				node = rb_first(&pvd->dirty_tree[hn.cpu]);
				p = rb_entry(node, struct tagged_page, node);

        if(trylock_tp(p, ULONG_MAX) != 0){ // failed to lock page
					spin_unlock(&pvd->tree_lock[hn.cpu]);
					cpu_relax();
					continue;
				}
				locked = true;
				DMAP_BGON(atomic_read(&p->in_use) == 0);
			
				min_heap_insert(mh, hn.cpu, p->page->index, p);
				rb_erase(node, &pvd->dirty_tree[hn.cpu]);
			}else
				locked = true;
			spin_unlock(&pvd->tree_lock[hn.cpu]);
#endif
		}

		/* add hn to an array */
		//printk(KERN_ERR "ADDING PAGE WITH OFFSET:[%lu]\n", hn.p->page->index);

		if(__page_cleanup != NULL)
			__page_cleanup(hn.p);
		pages[num_pages++] = hn.p;

		if(num_pages == max_pages){ /* TODO: This produces 2MB writes (512 pages * 4KB = 2MB) */
			if(pvd->type == D_BLKD)
				write_bio_async(pages, num_pages);
			else /* pvr->type == D_FILE */
				write_file_async(pages, num_pages);

			// unlock all
			for(i = 0; i < num_pages; ++i){
				unlock_tp(pages[i]);
			}
			
			num_pages = 0;
		}
	}

	if(num_pages > 0){
		if(pvd->type == D_BLKD)
			write_bio_async(pages, num_pages);
		else /* pvr->type == D_FILE */
			write_file_async(pages, num_pages);
		
		// unlock all
		for(i = 0; i < num_pages; ++i){
			unlock_tp(pages[i]);
		}
		
		num_pages = 0;
	}

	min_heap_destroy(mh);

	__free_page(pg);
#ifdef USE_RADIX_TREE_FOR_DIRTY
	free_page((unsigned long)__iter);
#endif

	return;
}
