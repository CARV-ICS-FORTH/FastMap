#ifndef _PERMA_HELPERS_H_
#define _PERMA_HELPERS_H_

#include <linux/mm.h> /* everything */
#include <asm/pgtable.h>

#include "tagged_page.h"
#include "shared_defines.h"

pte_t *get_pte(struct mm_struct *mm, unsigned long address);
void zap_page_from_address_space(struct vm_area_struct *vma, unsigned long address, struct tagged_page *tp, bool flush_tlb);
int unmap_page(struct page *page);
unsigned long scan_page_table(struct vm_area_struct *vma);
inline int is_mlocked_vma(struct vm_area_struct *vma);
inline int is_page_pinned(struct tagged_page *tagged_page);

#endif /* _PERMA_HELPERS_H_ */
