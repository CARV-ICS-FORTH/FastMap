#include <linux/mm.h>		/* everything */
#include <asm/tlbflush.h>
#include <linux/gfp.h>
#include <linux/sched.h>

#include "tagged_page.h"
#include "helpers.h"
#include "mmap_fault_handler.h"

#define GET_CPU_ID() (task_thread_info(current)->cpu)

extern void wake_up_page_bit(struct page *page, int bit_nr);

void clear_tagged_page_meta_data(struct tagged_page *tagged_page)
{
	int i;

	atomic_set(&tagged_page->in_use, 0);
	atomic_set(&tagged_page->page_valid, 0);
	tagged_page->page = NULL;

	atomic_set(&tagged_page->buffer_id, BI_NONE);

	spin_lock_init(&tagged_page->rmap_lock);
	for(i = 0; i < MAX_RMAPS_PER_PAGE; i++){

		tagged_page->rmap[i].__vma = NULL;
		tagged_page->rmap[i].__vaddr = 0;

		pvr_mk_invalid(&tagged_page->rmap[i]);
		pvr_set_idx(&tagged_page->rmap[i], i);
		pvr_set_vaddr(&tagged_page->rmap[i], 0);
		pvr_set_vma(&tagged_page->rmap[i], NULL);
		INIT_LIST_HEAD(&tagged_page->rmap[i].vma_mapped);
		pvr_set_cpu(&tagged_page->rmap[i], 0);
	}

#ifdef USE_WAITQUEUE_FOR_COLLISIONS
	init_waitqueue_head(&tagged_page->waiters);
#endif
}

void init_tagged_page_meta_data(struct tagged_page *tagged_page)
{
	clear_tagged_page_meta_data(tagged_page);
	atomic_set(&tagged_page->is_dirty, 0);
	atomic_set(&tagged_page->in_use, 0);
	spin_lock_init(&tagged_page->tp_lock_guard);
}

void alloc_tagged_page_data(struct tagged_page *tagged_page, int cpu)
{
	gfp_t gfp_flags;
	/*
	 * Must use NOIO because we don't want to recurse back into the
	 * block or filesystem layers from page reclaim.
	 *
	 * Cannot support XIP and highmem, because our ->direct_access
	 * routine for XIP must return memory that is always addressable.
	 * If XIP was reworked to use pfns and kmap throughout, this
	 * restriction might be able to be lifted.
	 */
	gfp_flags = GFP_NOIO; // | __GFP_ZERO |  __GFP_HIGHMEM;

	/* Allocate a page for storage */
	/* Note that the NUMA properties are for the current process that triggers the page allocation.  
	 * Typically this will be the process that interfaces with the sysfs, i.e. echo.  To get the desired
	 * NUMA allocation use a command such as: numactl -i 0-3 echo 1 > /sys/class/di-mmap-runtimeA/0_buffer_state
	 */
	tagged_page->page = alloc_pages_node(cpu_to_node(cpu), gfp_flags, 0);
	DMAP_BGON(tagged_page->page == NULL);
	SetPageReserved(tagged_page->page);

	tagged_page->page->private = (unsigned long)tagged_page;	

	/*
	 * Note that: Higher-order pages are called "compound pages".  They are structured thusly:
	 *
	 * The first PAGE_SIZE page is called the "head page".
	 *
	 * The remaining PAGE_SIZE pages are called "tail pages".
	 *
	 * All pages have PG_compound set.  All tail pages have their ->first_page
	 * pointing at the head page.
	 *
	 * The first tail page's ->lru.next holds the address of the compound page's
	 * put_page() function.  Its ->lru.prev holds the order of allocation.
	 * This usage means that zero-order pages may not be compound.
	 */
}

void free_tagged_page_data(struct tagged_page *tagged_page)
{
	DMAP_BGON(tagged_page == NULL);
	DMAP_BGON(tagged_page->page == NULL);

	ClearPageReserved(tagged_page->page);
	__free_pages(tagged_page->page, 0);
	tagged_page->page = NULL;
}

void print_tp(struct tagged_page *tp)
{
	printk(KERN_ERR "->[%d][%d][%d][%d]\n",
		atomic_read(&tp->page_valid),
		atomic_read(&tp->is_dirty),
		atomic_read(&tp->buffer_id),
		atomic_read(&tp->in_use)
	);
}

// #define pgoff_t unsigned long
// if index == ULONG_MAX ignore it
inline int trylock_tp(struct tagged_page *tp, pgoff_t index)
{
  /*
   * Atomic compare and exchange.  Compare OLD with MEM, if identical,
   * store NEW in MEM.  Return the initial value in MEM.  Success is
   * indicated by comparing RETURN with OLD.
   * cmpxchg(ptr, old, new)
   * int atomic_cmpxchg(atomic_t *v, int old, int new)
   */
	int ret = -1, in_use;

	spin_lock(&tp->tp_lock_guard);

	in_use = atomic_read(&tp->in_use);
	if(in_use != 1){
		spin_unlock(&tp->tp_lock_guard);
		return -3;
	}

	if(index != ULONG_MAX){
		if(tp->page->index != index){
			spin_unlock(&tp->tp_lock_guard);
			return -2;
		}
	}

	if(test_bit(PG_locked, &tp->page->flags) == 1){ // already locked
		ret = -1;
	}else if(test_bit(PG_locked, &tp->page->flags) == 0){
		set_bit(PG_locked, &tp->page->flags);
		ret = 0;
	}else{
		BUG();
	}

	spin_unlock(&tp->tp_lock_guard);
	return ret;
}

inline void unlock_tp(struct tagged_page *tp)
{
	int ret;

	spin_lock(&tp->tp_lock_guard);
	ret = clear_bit_unlock_is_negative_byte(PG_locked, &tp->page->flags);
	spin_unlock(&tp->tp_lock_guard);

	if (ret)
		wake_up_page_bit(tp->page, PG_locked);
}

inline int locked_tp(struct tagged_page *tp)
{
	return test_bit(PG_locked, &tp->page->flags);
}

int is_tagged_page_in_a_buffer(struct tagged_page *tagged_page, int buffer_id)
{
	return atomic_read(&tagged_page->buffer_id) == buffer_id;
}

/* Check if any of the pages within a tagged page are dirty */
inline int TaggedPageDirty(struct tagged_page *tagged_page)
{
	int is_dirty = 0;
	if(PageDirty(tagged_page->page))
		is_dirty = 1;

	return is_dirty || atomic_read(&tagged_page->is_dirty);
}

inline void ClearTaggedPageDirty(struct tagged_page *tagged_page)
{
	if(PageDirty(tagged_page->page))
		ClearPageDirty(tagged_page->page);
}

/* this is bit 0 in vma */
inline bool pvr_is_valid(struct pr_vma_rmap *p)
{
  return ((unsigned long)p->__vma & 0x01) == 1;
}

/* this is bit 0 in vma */
inline void pvr_mk_valid(struct pr_vma_rmap *p)
{
  p->__vma = (struct vm_area_struct *)((unsigned long)p->__vma | 0x01);
}

/* this is bit 0 in vma */
inline void pvr_mk_invalid(struct pr_vma_rmap *p)
{
  p->__vma = (struct vm_area_struct *)((unsigned long)p->__vma & ~(0x01));
}

/*  this is bits 6:0 in vaddr -> 7 bits -> 2^7 = 128 max CPUs */
inline unsigned long pvr_get_cpu(struct pr_vma_rmap *p)
{ 
	return p->__vaddr & 0x7F; 
}

inline void pvr_set_cpu(struct pr_vma_rmap *p, unsigned long cpu)
{
	p->__vaddr = p->__vaddr & ~(0x7F);
	p->__vaddr = p->__vaddr | cpu; 
}

/*  this is bit 11:7 in vaddr -> 5 bits -> 2^5 = 32 for MAX_RMAPS_PER_PAGE */
inline unsigned long pvr_get_idx(struct pr_vma_rmap *p)
{
	return (p->__vaddr & 0x0F80) >> 7;
}

inline void pvr_set_idx(struct pr_vma_rmap *p, unsigned long idx)
{
	const unsigned long cpu = p->__vaddr & 0x7F;
	const unsigned long vaddr = p->__vaddr & ~(0xFFF);
	
	p->__vaddr = (vaddr | (idx << 7) | cpu);
}

/* as vaddr is page aligned the 12 LS bits must be always 0 */
inline unsigned long pvr_get_vaddr(struct pr_vma_rmap *p)
{
	return p->__vaddr & ~(0xFFF); 
}

/* as vaddr is page aligned the 12 LS bits must be always 0 */
inline void pvr_set_vaddr(struct pr_vma_rmap *p, unsigned long vaddr)
{
  const unsigned long meta = p->__vaddr & 0xFFF;

  p->__vaddr = vaddr | meta;
}

/* check bit 0 before setting or getting vma */
inline struct vm_area_struct *pvr_get_vma(struct pr_vma_rmap *p)
{
  return (struct vm_area_struct *)((unsigned long)p->__vma & ~(0x01));
}

/* check bit 0 before setting or getting vma */
inline void pvr_set_vma(struct pr_vma_rmap *p, struct vm_area_struct *vma)
{
  const unsigned long valid = (unsigned long)p->__vma & 0x01;

  p->__vma = (struct vm_area_struct *)((unsigned long)vma | valid);
}

/* make some computation based on idx */
inline struct tagged_page *pvr_get_owner(struct pr_vma_rmap *p)
{
  const unsigned long idx = pvr_get_idx(p);

  return (struct tagged_page *)( (unsigned long)p - (idx * sizeof(struct pr_vma_rmap)) );
}

