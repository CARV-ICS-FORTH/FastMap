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
#include <linux/kdev_t.h>
#include <linux/rmap.h>

#include <asm/pgtable.h>
#include <asm/tlbflush.h>

#include "helpers.h"

void zap_page_from_address_space(struct vm_area_struct *vma, unsigned long address, struct tagged_page *tp, bool flush_tlb)
{
	struct page_vma_mapped_walk pvmw = {
		.page = tp->page,
		.vma = vma,
		.address = address,
		.flags = PVMW_SYNC,
	};

	DMAP_BGON(vma->vm_mm == NULL);

	pvmw.pte = NULL;
	pvmw.ptl = NULL;

	/* If the pte returned is valid, the lock will also be held. */
	while(page_vma_mapped_walk(&pvmw)){
		if(pvmw.pte){
			pte_t pteval = ptep_get_and_clear(vma->vm_mm, address, pvmw.pte);
			if(pte_dirty(pteval))
				SetPageDirty(tp->page);

			/* Update high watermark before we lower rss */
			update_hiwater_rss(vma->vm_mm);
			dec_mm_counter(vma->vm_mm, MM_FILEPAGES);
	
			if(flush_tlb)
				flush_tlb_page(pvmw.vma, pvmw.address); // also invalidate TLB entry
		}
	}
}

/*******************************************************************************
 * Implement variants of kernel code for implementing an mlocked page since the
 * original ones are not exported and are intextricably tied into the page cache.
 ******************************************************************************/

/*
 * from mm/internal.h
 * Called only in fault path via page_evictable() for a new page
 * to determine if it's being mapped into a LOCKED vma.
 * If so, mark page as mlocked.
 */
inline int is_mlocked_vma(struct vm_area_struct *vma)
{
	if (likely((vma->vm_flags & (VM_LOCKED | VM_SPECIAL)) != VM_LOCKED))
	{
		return 0;
	}
	return 1;
}

/**
 * Check to see if a page has been locked, or otherwise pinned in the user's memory.
 * This can be done by get_user_pages() and derivative functions, or mlock.
 */
inline int is_page_pinned(struct tagged_page *tagged_page)
{
	if (tagged_page == NULL)
		return 0;

#if 0
	if (PageUnevictable(tagged_page->page))
		return 1;

	if (PageMlocked(tagged_page->page))
		return 1;
#endif

	if (locked_tp(tagged_page) || PageLocked(tagged_page->page))
		return 1;

	return 0;
}
