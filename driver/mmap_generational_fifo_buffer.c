#include <asm/tlbflush.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/version.h>
#include <linux/blkdev.h>
#include <linux/vmalloc.h>
#include <linux/bio.h>
#include <linux/mm.h>
#include <linux/rmap.h>
#include <linux/hugetlb.h>
#include <linux/kthread.h>

#include "dmap.h"
#include "mmap_fault_handler.h"
#include "mmap_buffer_rbtree.h"
#include "mmap_generational_fifo_buffer.h"
#include "mmap_buffer_interface.h"
#include "mmap_buffer_hash_table.h"
#include "helpers.h"
#include "shared_defines.h"
#include "btree_metadata.h"

#ifdef USE_PROC_SYS_VM_COUNTERS
extern atomic_t mm_lf_stat_evicted_pages;
#endif

static atomic_t active_evictors;

extern void write_bio_tagged_page(struct tagged_page *evicted_page);
extern void write_file_tagged_page(struct tagged_page *evicted_page);

extern void write_bio_async(struct tagged_page **p, int len);
extern void write_file_async(struct tagged_page **p, int len);

struct lpfifo_operations_struct *lpfifo_ops = &lpfifo_operations;
struct dfifo_operations_struct *dfifo_ops = &dfifo_operations;

struct evictor_arg {
	int id;
	fifo_buffer_t *buf;
};

void clear_fifo_entry(struct tagged_page *tagged_page_to_clear, struct pr_vma_rmap *pvr, bool flush_tlb)
{
	if(likely(tagged_page_to_clear != NULL))
		zap_page_from_address_space(pvr_get_vma(pvr), pvr_get_vaddr(pvr), tagged_page_to_clear, flush_tlb);
}

/*
 * Take a single page that was removed from a buffer and place it in the victim queue.  
 * This is used by insert_page, drain buffer, and purge_select_pages_from_buffer.
 */
void drain_page(struct tagged_page *drained_page, fifo_buffer_t *buf, bool page_locked)
{
	struct pr_vma_entry *pve;
#ifdef USE_PERCPU_RADIXTREE
	const unsigned int cpus = num_online_cpus();
	unsigned int radix_tree_id;
#endif
	int i;
	struct pr_vma_data *pvd = drained_page->pvd;

	if(unlikely(drained_page == NULL))
		return;

	if(!page_locked){
		while(trylock_tp(drained_page, ULONG_MAX) != 0)
			wait_on_page_bit(drained_page->page, PG_locked);
		DMAP_BGON(atomic_read(&drained_page->in_use) == 0);
	}

#ifdef USE_PERCPU_RADIXTREE
	radix_tree_id = drained_page->page->index % cpus;
	spin_lock(&pvd->radix_lock[radix_tree_id]);
	radix_tree_delete(&pvd->rdx[radix_tree_id], drained_page->page->index);
	spin_unlock(&pvd->radix_lock[radix_tree_id]);
#else
	spin_lock(&pvd->radix_lock);
	radix_tree_delete(&pvd->rdx, drained_page->page->index);
	spin_unlock(&pvd->radix_lock);
#endif

	spin_lock(&drained_page->rmap_lock);
	for(i = 0; i < MAX_RMAPS_PER_PAGE; i++){
		struct pr_vma_rmap *pvr = &(drained_page->rmap[i]);
		if(pvr_is_valid(pvr) == false)
			continue;

		clear_fifo_entry(drained_page, pvr, true);
		pvr_mk_invalid(pvr);

		pve = ((struct fastmap_info *)pvr_get_vma(pvr)->vm_private_data)->pve;
		DMAP_BGON(pve == NULL);

		spin_lock(&pve->pcpu_mapped_lock[pvr_get_cpu(pvr)]);
		list_del(&pvr->vma_mapped);
		spin_unlock(&pve->pcpu_mapped_lock[pvr_get_cpu(pvr)]);
	}
	spin_unlock(&drained_page->rmap_lock);

	DMAP_BGON(atomic_read(&drained_page->is_dirty) == 1);

	free_page_lock(&(buf->page_map), drained_page); /* it requires the page locked */
}

void reset_params_fifo_buffer_t(fifo_buffer_t *buf){}

/* 
 * This function changes a read-write mapping to a read-only
 * mapping.
 */
void clear_mappings(struct tagged_page *p, struct evictor_tlb *etlb)
{
	unsigned int i;
	int k;

	spin_lock(&p->rmap_lock);
	for(i = 0; i < MAX_RMAPS_PER_PAGE; i++){
		struct pr_vma_rmap *pvr = &(p->rmap[i]);
		if(pvr_is_valid(pvr) == false)
			continue;

#if 0
		/* FIXME we have to use page_vma_mapped_walk here */
		pte = get_pte(pvr_get_vma(pvr)->vm_mm, pvr_get_vaddr(pvr));
		if(pte != NULL && !pte_none(*pte)){
			*pte = pte_mkclean(*pte);
			*pte = pte_wrprotect(*pte);
#else
		{
			struct page_vma_mapped_walk pvmw = {
				.page = p->page,
				.vma = pvr_get_vma(pvr),
				.address = pvr_get_vaddr(pvr),
				.flags = PVMW_SYNC,
			};

			pvmw.pte = NULL;
			pvmw.ptl = NULL;

			DMAP_BGON(pvmw.vma->vm_mm == NULL);

			/* If the pte returned is valid, the lock will also be held. */
			while(page_vma_mapped_walk(&pvmw)){
				if(pvmw.pte){
					pte_t entry;
					pte_t *pte = pvmw.pte;

					entry = READ_ONCE(*pte);
					entry = pte_mkclean(entry);
					entry = pte_mkyoung(entry);
					entry = pte_wrprotect(entry);
					set_pte_at(pvmw.vma->vm_mm, pvmw.address, pte, entry);
				}
			}
#endif
			if(etlb == NULL){
				flush_tlb_page(pvr_get_vma(pvr), pvr_get_vaddr(pvr));
			}else{
				int empty = -1;
				bool done = false;

				for(k = 0; k < ETLB_ENTRIES; k++){
					if(etlb[k].vma == pvr_get_vma(pvr)){
						if(pvr_get_vaddr(pvr) > etlb[k].start)
							etlb[k].start = pvr_get_vaddr(pvr);

						if(pvr_get_vaddr(pvr) < etlb[k].end)
							etlb[k].end = pvr_get_vaddr(pvr);

						done = true;
						break;
					}

					if(etlb[k].vma == NULL && empty == -1)
						empty = k;
				}

				if(done == false){
					DMAP_BGON(empty == -1);
					etlb[empty].vma = pvr_get_vma(pvr);
					etlb[empty].start = pvr_get_vaddr(pvr);
					etlb[empty].end = pvr_get_vaddr(pvr);
				}
			} /* else -- etlb != NULL */

		}
	}
	spin_unlock(&p->rmap_lock);		
}

static void remove_from_dirty_tree(struct tagged_page *p)
{
	unsigned int dirty_tree_id = p->page->index % num_online_cpus();;

	spin_lock(&p->pvd->tree_lock[dirty_tree_id]);
#ifdef USE_RADIX_TREE_FOR_DIRTY
	radix_tree_delete(&(p->pvd->dirty_tree[dirty_tree_id]), p->page->index);	
#else
	tagged_rb_erase(&p->pvd->dirty_tree[dirty_tree_id], p->page->index);
#endif
	spin_unlock(&p->pvd->tree_lock[dirty_tree_id]);
}

static int ev_deq(struct tagged_page *p)
{
	DMAP_BGON(p == NULL);

	if(trylock_tp(p, ULONG_MAX) != 0) // failed to lock page
    return 0;

	DMAP_BGON(atomic_read(&p->in_use) == 0);
  return 1;
}

static int evictor_thread(void *__buf)
{
	struct evictor_arg *args = (struct evictor_arg *)__buf;
	fifo_buffer_t *buf = args->buf;
	int ID = args->id;
	dfifo_t *dirty_data = buf->dirty_queue[ID];
	struct tagged_page *tp;
	const unsigned long buffer_size = buf->primary_fifo_buffer_size;
	unsigned int dirty_size, dirty_tree_id;
	pgoff_t offset;

	/* more than we need */
	struct evictor_tlb *etlb = page_address(alloc_page(GFP_KERNEL));

	/* this is 2MB */
	struct tagged_page **pgs = page_address(alloc_page(GFP_KERNEL));
	unsigned int i, iter, num_pgs = 0;
	int k;

	struct pr_vma_data *pvd = NULL;

	printk(KERN_ERR "Starting evictor with ID = %d\n", ID);

	while(1){
		if(buf->evictor_run == false){
			free_page((long unsigned int)pgs);
			free_page((long unsigned int)etlb);
			free_page((long unsigned int)__buf);
			printk(KERN_ERR "Stopping evictor with ID = %d\n", ID);
			atomic_dec(&active_evictors);
			do_exit(0);
		}

		//printk(KERN_ERR "clean size = %lu, dirty size = %lu, buffer size = %lu\n", 
		//					buf->primary_fifo_data->size, 
		//					buf->dirty_queue[ID]->size, 
		//					buffer_size);

#if 0
		if( buf->dirty_queue[ID]->size < ((buffer_size / EVICTOR_THREADS) / 2) ){
			ssleep(1);
			continue;
		}
#else
		dirty_size = 0;
		for(i = 0; i < EVICTOR_THREADS; ++i)
			dirty_size += buf->dirty_queue[i]->size;

		if( dirty_size < ((buffer_size / 5) * 3) ){
			ssleep(1);
			continue;
		}
#endif

		for(k = 0; k < ETLB_ENTRIES; k++){
			etlb[k].vma = NULL;
			etlb[k].start = 0;
			etlb[k].end = UINT_MAX;
		}

		/*
		 * 0) if(dirty_size > threashold) do the following
		 * 1) dequeue a dirty page from dirty queue
		 * 2) based on dirty trees find neighbour pages and remove thes from the dirty queue
		 * 3) do the I/O
		 * 4) as these pages are mapped change also the mapping to be clean
		 * 5) add all clean pages to clean queue
		 */

		spin_lock(&dirty_data->dlock);
		pgs[0] = dfifo_ops->dequeue_if(dirty_data, ev_deq);
		spin_unlock(&dirty_data->dlock);
		if(pgs[0] == NULL){
			ssleep(1);
			continue;
		}

		num_pgs = 1;
		pvd = pgs[0]->pvd;
		remove_from_dirty_tree(pgs[0]);

		iter = 0;
		offset = pgs[0]->page->index + 1;
		while(num_pgs < 512)
		{
			tp = NULL;
			dirty_tree_id = offset % num_online_cpus();

			/* XXX The following if statement ensures that this evictor thread accesses 
			 * only pages that belongs to it and the corresponding dirty queue. If not 
			 * there can be multiple threads that access the same pages from the dirty trees 
			 * and this could cause several races.
			 */
			if( ((offset >> 9) % EVICTOR_THREADS) != ID ) /* this belongs to another evictor thread */
				break;

#ifdef USE_RADIX_TREE_FOR_DIRTY
			rcu_read_lock();
			tp = radix_tree_lookup(&(pvd->dirty_tree[dirty_tree_id]), offset);
			if(tp != NULL){
				spin_lock(&pvd->tree_lock[dirty_tree_id]);	
				radix_tree_delete(&(pvd->dirty_tree[dirty_tree_id]), offset);
				pgs[num_pgs++] = tp;
				spin_unlock(&pvd->tree_lock[dirty_tree_id]);
			}
			rcu_read_unlock();
#else
			spin_lock(&pvd->tree_lock[dirty_tree_id]);
			tp = tagged_rb_search(&pvd->dirty_tree[dirty_tree_id], offset);
			if(tp != NULL){
				rb_erase(&tp->node, &pvd->dirty_tree[dirty_tree_id]);	
				pgs[num_pgs++] = tp;
			}
			spin_unlock(&pvd->tree_lock[dirty_tree_id]);
#endif

			if(tp != NULL){
				int ret; 

				spin_lock(&dirty_data->dlock);
				ret = dfifo_ops->remove_if(dirty_data, tp, ev_deq);
				spin_unlock(&dirty_data->dlock);

				if(ret == 0){ /* cannot remove the page */
#ifdef USE_RADIX_TREE_FOR_DIRTY
					int retr = -1;
#endif
					spin_lock(&pvd->tree_lock[dirty_tree_id]);
#ifdef USE_RADIX_TREE_FOR_DIRTY
					retr = radix_tree_insert(&pvd->dirty_tree[dirty_tree_id], tp->page->index, tp);
					BUG_ON(retr != 0);
#else
					tagged_rb_insert(&pvd->dirty_tree[dirty_tree_id], tp);	
#endif
					spin_unlock(&pvd->tree_lock[dirty_tree_id]);
					num_pgs--;
					pgs[num_pgs] = NULL;
				}
			}
		
			DMAP_BGON(num_pgs < 1);
			offset++;
			if(iter++ >= 2048)
				break;
		}

		/* Now we have some pages that:
		 * - are locked
		 * - removed from dirty tree
		 * - removed from the queue
		 * - are sequential (as much as we can)
		 */

		for(i = 0; i < num_pgs; ++i){ /* check that all pages are locked here */
			//DMAP_BGON(!PageLocked(pgs[i]->page));
			DMAP_BGON(!locked_tp(pgs[i]));
		}

		/* clear all mappings */
		for(i = 0; i < num_pgs; ++i) 
			clear_mappings(pgs[i], etlb);

		for(k = 0; k < ETLB_ENTRIES; k++)
			if(etlb[k].vma != NULL)
				flush_tlb_mm_range(etlb[k].vma->vm_mm, etlb[k].start, etlb[k].end + PAGE_SIZE, VM_NONE);

		/* write them */
		if(pvd->type == D_BLKD)
			write_bio_async(pgs, num_pgs);
		else /* p->type == D_FILE */
			write_file_async(pgs, num_pgs);
		
		/* add to clean queue */
		for(i = 0; i < num_pgs; ++i)
			insert_page_fifo_buffer_t(buf, pgs[i], true); /* and clear pages (true) */
		
		//for(i = 0; i < num_pgs; ++i){
		//	atomic_set(&pgs[i]->is_dirty, 0);
		//	ClearTaggedPageDirty(pgs[i]);
		//}

		for(i = 0; i < num_pgs; ++i)
			unlock_tp(pgs[i]); /* XXX unlock all */

#ifdef USE_PROC_SYS_VM_COUNTERS
		atomic_add(num_pgs, &mm_lf_stat_evicted_pages);
#endif
	}

	return 0;
}

int init_params_fifo_buffer_t(fifo_buffer_t *buf, buf_ld_params_t *ld_params)
{
	unsigned long mmap_buf_size = ld_params->mmap_buf_size;
	long num_free_pages = 0;
	int i, err = -ENOMEM;
	struct evictor_arg *args[EVICTOR_THREADS];

	copy_ld_params(&buf->ld_params, ld_params); /* Propogate the necessary parameters into the embedded queues */
	err = 0;

	/* Leave > 1/8 of the space for the primary fifo */
	buf->primary_fifo_buffer_size = mmap_buf_size;
	DMAP_BGON(buf->primary_fifo_buffer_size < 2);

	/* Allocate a buffer if the size is none zero and there isn't an existing buffer */	
	/* FIXME: The queue can be larger by the number of cpus because of a race. Can we solve this in 
	 * a more elegant way? */
	for(i = 0; i < NUM_QUEUES; ++i)
		lpfifo_ops->init(buf->primary_fifo_data[i], buf->primary_fifo_buffer_size + num_online_cpus());

	for(i = 0; i < EVICTOR_THREADS; ++i){
		dfifo_ops->init(buf->dirty_queue[i], buf->primary_fifo_buffer_size + num_online_cpus());

		args[i] = (struct evictor_arg *)page_address(alloc_page(GFP_KERNEL));
		args[i]->id = i;
		args[i]->buf = buf;
	}

	num_free_pages += buf->primary_fifo_buffer_size;

	printk(KERN_ERR "|TOTAL|PRIMARY|FREE|=|%ld|%ld|%ld|\n", 
			mmap_buf_size, buf->primary_fifo_buffer_size, 
			num_free_pages);

	printk(KERN_ERR "num_banks = %d, total_buffer = %luMB\n", 1, (num_free_pages * 4096) / 1024 / 1024);

	init_hash_table_free_pages(&buf->page_map, num_free_pages);

	buf->page_map.owner = buf;

	atomic_set(&active_evictors, 0);
	buf->evictor_run = true;
	for(i = 0; i < EVICTOR_THREADS; ++i){
		atomic_inc(&active_evictors);
		buf->evictor[i] = kthread_run(evictor_thread, args[i], "fmap-evict%d", i);
	}

	return err;
}

/* 
 * This function removes all pages from queue
 */
void cleanup_params_fifo_buffer_t(fifo_buffer_t *buf)
{
	unsigned long i, count = 0;
	struct tagged_page *tagged_page;

	printk(KERN_ERR "[%s:%s:%d] Starting to cleaning primary queue\n", __FILE__, __func__, __LINE__);

	for(i = 0; i < NUM_QUEUES; ++i){
		while(1){
			spin_lock(&buf->primary_fifo_data[i]->qlock);
			tagged_page = lpfifo_ops->dequeue(buf->primary_fifo_data[i]);
			spin_unlock(&buf->primary_fifo_data[i]->qlock);
			if(tagged_page == NULL)
				break;

			drain_page(tagged_page, buf, false);
			count++;
		}
	}

	printk(KERN_ERR "[%s:%s:%d][%lu] Done with cleaning primary queue\n", __FILE__, __func__, __LINE__, count);
}

void try_purge_pages(fifo_buffer_t *buf, unsigned long target, int queue_id)
{
	lpfifo_t *fifo_data = buf->primary_fifo_data[queue_id];
	struct tagged_page *p;
	unsigned long num = 0;
	unsigned long iter = 0;

	while(1){
		if(++iter >= target)
			return;

retry:
		spin_lock(&buf->primary_fifo_data[queue_id]->qlock);
		p = lpfifo_ops->dequeue(fifo_data);
		if(p == NULL){
			spin_unlock(&buf->primary_fifo_data[queue_id]->qlock);
			return;
		}

		if(is_page_pinned(p)){
			lpfifo_ops->enqueue(fifo_data, p);
			spin_unlock(&buf->primary_fifo_data[queue_id]->qlock);
			goto retry;
		}

		if(trylock_tp(p, ULONG_MAX) != 0){ // failed to lock page
			lpfifo_ops->enqueue(fifo_data, p);
			spin_unlock(&buf->primary_fifo_data[queue_id]->qlock);
			goto retry;
		}

		spin_unlock(&buf->primary_fifo_data[queue_id]->qlock);
		DMAP_BGON(atomic_read(&p->in_use) == 0);

		DMAP_BGON(atomic_read(&p->is_dirty) == 1);
		drain_page(p, buf, true);
		if(++num == target)
			break;
	}
}

void remove_mappings(struct tagged_page *p, struct evictor_tlb *etlb)
{
	unsigned int i, k;
	struct pr_vma_entry *pve;

	spin_lock(&p->rmap_lock);
	for(i = 0; i < MAX_RMAPS_PER_PAGE; i++){
		struct pr_vma_rmap *pvr = &(p->rmap[i]);
		if(pvr_is_valid(pvr) == false)
			continue;

		if(etlb == NULL)
			clear_fifo_entry(p, pvr, true);
		else
			clear_fifo_entry(p, pvr, false);

		if(etlb != NULL){ // update etlb
			int empty = -1;
			bool done = false;

			for(k = 0; k < ETLB_ENTRIES; k++){
				if(etlb[k].vm_mm == pvr_get_vma(pvr)->vm_mm){
					if(pvr_get_vaddr(pvr) > etlb[k].start)
						etlb[k].start = pvr_get_vaddr(pvr);

					if(pvr_get_vaddr(pvr) < etlb[k].end)
						etlb[k].end = pvr_get_vaddr(pvr);

					done = true;
					break;
				}

				if(etlb[k].vm_mm == NULL && empty == -1)
					empty = k;
			}

			if(done == false){
				DMAP_BGON(empty == -1);
				etlb[empty].vm_mm = pvr_get_vma(pvr)->vm_mm;
				etlb[empty].start = pvr_get_vaddr(pvr);
				etlb[empty].end = pvr_get_vaddr(pvr);
			}
		}
		pvr_mk_invalid(pvr);

		pve = ((struct fastmap_info *)pvr_get_vma(pvr)->vm_private_data)->pve;
		DMAP_BGON(pve == NULL);

		spin_lock(&pve->pcpu_mapped_lock[pvr_get_cpu(pvr)]);
		list_del(&pvr->vma_mapped);
		spin_unlock(&pve->pcpu_mapped_lock[pvr_get_cpu(pvr)]);
	}
	spin_unlock(&p->rmap_lock);
}

static int deq(struct tagged_page *p)
{
	DMAP_BGON(p == NULL);

	if(atomic64_read(&p->pvd->during_unlink) == 1)
		return 0;

	if(trylock_tp(p, ULONG_MAX) != 0) /* failed to lock page */
		return 0;

	DMAP_BGON(atomic_read(&p->in_use) == 0);
	return 1;
}

/*
 * This function tries to remove N pages from the buffer. As we can
 * have multiple queues per buffer this function operates only on a
 * specific queue (@param qid). It takes the buffer as the first
 * argument (@param buf). It handles the updates to the page table
 * the TLB manipulation. Finally it returs the number of pages that
 * it successfully removed from the buffer.
 */
unsigned int try_purge_pages_fast(fifo_buffer_t *buf, unsigned int N, int qid)
{
	struct tagged_page *p;
	struct pr_vma_data *pvd;
	lpfifo_t *fifo_data = buf->primary_fifo_data[qid];
	struct evictor_tlb etlb[ETLB_ENTRIES];
	unsigned int i, num_pgs = 0;
#ifdef USE_PERCPU_RADIXTREE
	const unsigned int cpus = num_online_cpus();
	unsigned int radix_tree_id;
#endif
	//struct tagged_page **pgs = page_address(alloc_page(GFP_KERNEL));
	struct tagged_page **pgs = vmalloc(192 * sizeof(struct tagged_page *));
	DMAP_BGON(pgs == NULL);

	N = 192;

	for(i = 0; i < ETLB_ENTRIES; i++){
		etlb[i].vm_mm = NULL;
		etlb[i].start = 0;
		etlb[i].end = UINT_MAX;
	}

	// Remove N pages from the queue, from the radix and from page_tables.
	for(i = 0; i < N; ++i){
		spin_lock(&fifo_data->qlock);
		p = lpfifo_ops->dequeue_if(fifo_data, deq);
		spin_unlock(&fifo_data->qlock);

		if(p == NULL)
			continue;

		pvd = p->pvd;

#ifdef USE_PERCPU_RADIXTREE
		radix_tree_id = p->page->index % cpus; 
		spin_lock(&pvd->radix_lock[radix_tree_id]);
		radix_tree_delete(&(pvd->rdx[radix_tree_id]), p->page->index);
		spin_unlock(&pvd->radix_lock[radix_tree_id]);
#else
		spin_lock(&pvd->radix_lock);
		radix_tree_delete(&pvd->rdx, p->page->index);
		spin_unlock(&pvd->radix_lock);
#endif

		remove_mappings(p, etlb);

		pgs[num_pgs] = p;
		num_pgs++;
	}

	if(num_pgs == 0)
		return 0;

	// Flush TLB for each vma produced from the previous step.
	for(i = 0; i < ETLB_ENTRIES; i++)
		if(etlb[i].vm_mm != NULL)
			flush_tlb_mm_range(etlb[i].vm_mm, etlb[i].start, etlb[i].end + PAGE_SIZE, VM_NONE);

	// Unlock and deallocate all pages
	for(i = 0; i < num_pgs; ++i){
		p = pgs[i];
		free_page_lock(&(buf->page_map), p); /* it requires the page locked */
	}

	//free_page((long unsigned int)pgs);

	vfree(pgs);
	return num_pgs;
}

void insert_page_fifo_buffer_t(fifo_buffer_t *buf, struct tagged_page *tagged_page, bool clear)
{
	int ret;
	lpfifo_t *fifo_data = buf->primary_fifo_data[tagged_page->page->index % NUM_QUEUES];

	DMAP_BGON(fifo_data == NULL);
	DMAP_BGON(tagged_page == NULL);
	
	DMAP_BGON(atomic_read(&tagged_page->buffer_id) != BI_NONE);

	spin_lock(&buf->primary_fifo_data[tagged_page->page->index % NUM_QUEUES]->qlock);

	DMAP_BGON(lpfifo_ops->fifo_full(fifo_data));
	ret = lpfifo_ops->enqueue(fifo_data, tagged_page);
	
	if(clear){
		atomic_set(&tagged_page->is_dirty, 0);
		ClearTaggedPageDirty(tagged_page);
	}

	spin_unlock(&buf->primary_fifo_data[tagged_page->page->index % NUM_QUEUES]->qlock);
	
	DMAP_BGON(atomic_read(&tagged_page->buffer_id) != BI_CLEAN);
	DMAP_BGON(ret != 0);
}

/* clean queue -> dirty queue */
void move_page_c2d_fifo_buffer_t(fifo_buffer_t *buf, struct tagged_page *tagged_page)
{
	/* 
	 * 512 consequtive pages in the same queue to enable 
	 * effective merging (2^9 = 512).
	 */
	int ret, ID = (tagged_page->page->index >> 9) % EVICTOR_THREADS;
	lpfifo_t *clean_data = buf->primary_fifo_data[tagged_page->page->index % NUM_QUEUES];
	dfifo_t *dirty_data = buf->dirty_queue[ID];

	DMAP_BGON(clean_data == NULL);
	DMAP_BGON(dirty_data == NULL);
	DMAP_BGON(tagged_page == NULL);

	DMAP_BGON(atomic_read(&tagged_page->buffer_id) != BI_CLEAN);

	spin_lock(&clean_data->qlock);
	lpfifo_ops->remove(clean_data, tagged_page);
	spin_unlock(&clean_data->qlock);

	spin_lock(&dirty_data->dlock);
	if(dfifo_ops->fifo_full(dirty_data)){
		printk("ID = %d, size = %lu, max_size = %lu\n", ID, dirty_data->size, dirty_data->max_size);
		DMAP_BGON(1);
	}
	ret = dfifo_ops->enqueue(dirty_data, tagged_page);
	spin_unlock(&dirty_data->dlock);

	DMAP_BGON(atomic_read(&tagged_page->buffer_id) != BI_DIRTY);

	DMAP_BGON(ret != 0);
}

/* dirty queue -> clean queue */
void move_page_d2c_fifo_buffer_t(fifo_buffer_t *buf, struct tagged_page *tagged_page)
{
	/* 
	 * 512 consequtive pages in the same queue to enable 
	 * effective merging (2^9 = 512).
	 */
	int ret, ID = (tagged_page->page->index >> 9) % EVICTOR_THREADS;
	lpfifo_t *clean_data = buf->primary_fifo_data[tagged_page->page->index % NUM_QUEUES];
	dfifo_t *dirty_data = buf->dirty_queue[ID];

	DMAP_BGON(clean_data == NULL);
	DMAP_BGON(dirty_data == NULL);
	DMAP_BGON(tagged_page == NULL);

	DMAP_BGON(atomic_read(&tagged_page->buffer_id) != BI_DIRTY);

	spin_lock(&dirty_data->dlock);
	dfifo_ops->remove(dirty_data, tagged_page);
	spin_unlock(&dirty_data->dlock);

	spin_lock(&clean_data->qlock);
	DMAP_BGON(lpfifo_ops->fifo_full(clean_data));
	ret = lpfifo_ops->enqueue(clean_data, tagged_page);
	atomic_set(&tagged_page->is_dirty, 0);
	ClearTaggedPageDirty(tagged_page);
	spin_unlock(&clean_data->qlock);
	
	DMAP_BGON(atomic_read(&tagged_page->buffer_id) != BI_CLEAN);

	DMAP_BGON(ret != 0);
}

void hit_page_fifo_buffer_t(fifo_buffer_t *buf, struct tagged_page *tagged_page)
{
	int retval;

	spin_lock(&buf->primary_fifo_data[tagged_page->page->index % NUM_QUEUES]->qlock);
	lpfifo_ops->remove(buf->primary_fifo_data[tagged_page->page->index % NUM_QUEUES], tagged_page);
	retval = lpfifo_ops->enqueue(buf->primary_fifo_data[tagged_page->page->index % NUM_QUEUES], tagged_page);
	spin_unlock(&buf->primary_fifo_data[tagged_page->page->index % NUM_QUEUES]->qlock);
	DMAP_BGON(retval != 0);
}

int change_page_priority_fifo_buffer_t(fifo_buffer_t *buf, pgoff_t physical_addr, struct pr_vma_data *pvd, uint8_t new_pri)
{
	struct tagged_page *tagged_page;
#ifdef USE_PERCPU_RADIXTREE
	const unsigned int cpus = num_online_cpus();
	unsigned int radix_tree_id;
#endif

	rcu_read_lock();
#ifdef USE_PERCPU_RADIXTREE
	radix_tree_id = physical_addr % cpus;
	tagged_page = radix_tree_lookup(&(pvd->rdx[radix_tree_id]), physical_addr);
#else
	tagged_page = radix_tree_lookup(&(pvd->rdx), physical_addr);
#endif
	if(unlikely(tagged_page == NULL)){
		rcu_read_unlock();
		return 0;
	}

	spin_lock(&buf->primary_fifo_data[tagged_page->page->index % NUM_QUEUES]->qlock);
	lpfifo_ops->change_priority(buf->primary_fifo_data[tagged_page->page->index % NUM_QUEUES], new_pri, tagged_page);
	spin_unlock(&buf->primary_fifo_data[tagged_page->page->index % NUM_QUEUES]->qlock);

	rcu_read_unlock();

	return 1;
}

void reset_state_fifo_buffer_t(fifo_buffer_t *buf)
{
	int i;

	for(i = 0; i < NUM_QUEUES; ++i)
		lpfifo_ops->reset(buf->primary_fifo_data[i]);
}

void reset_history_fifo_buffer_t(fifo_buffer_t *buf){}

int update_runtime_parameters_fifo_buffer_t(fifo_buffer_t *buf, buf_rt_params_t *rt_params)
{
	return copy_rt_params(&buf->rt_params, rt_params);
}

void init_specific_counters_fifo_buffer_t(fifo_buffer_t *buf)
{
	//atomic64_set(&num_write_eviction_races, 0);
	return;
}

/* 
 * Use the timestamp (buffer location) and buffer id values for each page to determine if it is in a buffer.
 * In the case of one page being evicted from a buffer, while another one is being inserted, there is a condition
 * where the evicted page still thinks that it is in the buffer, but the new page doesn't yet report that it is in
 * that slot as long as the locks are nested.
 */
int page_in_buffer_fifo_buffer_t(fifo_buffer_t *buf, struct tagged_page *tagged_page)
{
	return is_tagged_page_in_a_buffer(tagged_page, BI_CLEAN) || is_tagged_page_in_a_buffer(tagged_page, BI_DIRTY);
}

extern int vtagged_rb_contains(struct rb_root *root, struct tagged_page *data);

fifo_buffer_t *init_mmap_buffer_data_fifo_buffer_t(fifo_buffer_t *dummy, buf_ld_params_t *ld_params, int bank_id)
{
	int i;
	fifo_buffer_t *buf;

	buf = (fifo_buffer_t *)kmalloc(sizeof(fifo_buffer_t), GFP_KERNEL | __GFP_ZERO);
	DMAP_BGON(buf == NULL);
	memset(buf, 0, sizeof(fifo_buffer_t));

	for(i = 0; i < NUM_QUEUES; ++i){
		buf->primary_fifo_data[i] = (lpfifo_t *)kmalloc(sizeof(lpfifo_t), GFP_KERNEL | __GFP_ZERO);
		DMAP_BGON(buf->primary_fifo_data[i] == NULL);
		memset(buf->primary_fifo_data[i], 0, sizeof(lpfifo_t));
	}

	for(i = 0; i < EVICTOR_THREADS; ++i){
		buf->dirty_queue[i] = (dfifo_t *)kmalloc(sizeof(dfifo_t), GFP_KERNEL | __GFP_ZERO);
		DMAP_BGON(buf->dirty_queue[i] == NULL);
		memset(buf->dirty_queue[i], 0, sizeof(dfifo_t));
	}

	init_ld_params(&buf->ld_params);
	copy_ld_params(&buf->ld_params, ld_params);
	init_rt_params(&buf->rt_params);

	/* Initialize a mutex for the entire buffer */
	spin_lock_init(&buf->buffer_lock);

	return buf;
}

void cleanup_mmap_buffer_data_fifo_buffer_t(fifo_buffer_t *buf)
{
	int i;
	buf->evictor_run = false;
	while(atomic_read(&active_evictors) != 0)
		ssleep(1);
	ssleep(1); /* wait a bit more for everything to complete */

	free_hash_table(&buf->page_map);

	for(i=0; i < NUM_QUEUES; i++)
		kfree(buf->primary_fifo_data[i]);
	for(i=0; i < EVICTOR_THREADS; i++)
		kfree(buf->dirty_queue[i]);
	kfree(buf);
}

void print_status_fifo_buffer_t(fifo_buffer_t *buf){}
