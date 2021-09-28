#ifndef _TAGGED_PAGE_H_
#define _TAGGED_PAGE_H_

#include <linux/mm.h> 
#include <linux/types.h>
#include <linux/list.h>
#include <linux/rbtree.h>

#include "shared_defines.h"

/* defines for buffer_id */
#define BI_NONE		0x00
#define BI_CLEAN	0x01
#define BI_DIRTY	0x02

struct pr_vma_rmap
{
  unsigned long __vaddr; /* virtual address of mapping */
  struct vm_area_struct *__vma; /* struct mm_struct *vm_mm -- contained here */
  struct list_head vma_mapped; /* struct list_head pcpu_mapped */
};

struct tagged_page
{
  struct pr_vma_rmap rmap[MAX_RMAPS_PER_PAGE]; /* reverse mappings */ /* XXX this should be first */
  spinlock_t rmap_lock; /* lock for rmap */

#ifndef USE_RADIX_TREE_FOR_DIRTY
  struct rb_node node; /* dirty pages reside in a red-black tree */
#endif
  struct page *page; /* pointer to the physical page */
  struct pr_vma_data *pvd; /* pointer to the pvd (underlying block device or file) */

  struct list_head qlist; /* queue list (LRU) */
  struct list_head free; /* free list */

  atomic_t page_valid;
  atomic_t is_dirty;
	atomic_t buffer_id;
	atomic_t in_use;

	spinlock_t tp_lock_guard;

  uint32_t priority;
  uint32_t cpuid; /* free-list ID */

#ifdef USE_WAITQUEUE_FOR_COLLISIONS
  wait_queue_head_t waiters;
#endif
} ____cacheline_aligned;

void print_tp(struct tagged_page *tp);

inline int trylock_tp(struct tagged_page *tp, pgoff_t index);
inline void unlock_tp(struct tagged_page *tp);
inline int locked_tp(struct tagged_page *tp);

/* this is bit 0 in vma */
inline bool pvr_is_valid(struct pr_vma_rmap *p);
inline void pvr_mk_valid(struct pr_vma_rmap *p);
inline void pvr_mk_invalid(struct pr_vma_rmap *p);

/*  this is bits 6:0 in vaddr -> 7 bits -> 2^7 = 128 max CPUs */
inline unsigned long pvr_get_cpu(struct pr_vma_rmap *p);
inline void pvr_set_cpu(struct pr_vma_rmap *p, unsigned long cpu);

/*  this is bit 11:7 in vaddr -> 5 bits -> 2^5 = 32 for MAX_RMAPS_PER_PAGE */
inline unsigned long pvr_get_idx(struct pr_vma_rmap *p);
inline void pvr_set_idx(struct pr_vma_rmap *p, unsigned long idx);

/* as vaddr is page aligned the 12 LS bits must be always 0 */
inline unsigned long pvr_get_vaddr(struct pr_vma_rmap *p);
inline void pvr_set_vaddr(struct pr_vma_rmap *p, unsigned long vaddr);

/* check bit 0 before setting or getting vma */
inline struct vm_area_struct *pvr_get_vma(struct pr_vma_rmap *p);
inline void pvr_set_vma(struct pr_vma_rmap *p, struct vm_area_struct *vma);

/* make some computation based on idx */
inline struct tagged_page *pvr_get_owner(struct pr_vma_rmap *p);

void clear_tagged_page_meta_data(struct tagged_page *tagged_page);
void init_tagged_page_meta_data(struct tagged_page *tagged_page);
void alloc_tagged_page_data(struct tagged_page *tagged_page, int cpu);
void free_tagged_page_data(struct tagged_page *tagged_page);
int is_tagged_page_in_a_buffer(struct tagged_page *tagged_page, int buffer_id);
int TaggedPageDirty(struct tagged_page *tagged_page);
void ClearTaggedPageDirty(struct tagged_page *tagged_page);

#endif /* _TAGGED_PAGE_H_ */
