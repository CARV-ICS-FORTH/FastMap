#include <linux/kallsyms.h>
#include <linux/fs.h>
#include <linux/mm_types.h>
#include <linux/types.h>
#include <linux/compiler_types.h>
#include <linux/rmap.h>

/* Auxiliary function pointers here */

void (*flush_tlb_mm_range_ksym)(struct mm_struct *mm, unsigned long start,
 	unsigned long end, unsigned long vmflag);

ssize_t (*vfs_read_ksym)(struct file *file, char __user *buf, 
 	size_t count, loff_t *pos);

ssize_t (*vfs_write_ksym)(struct file *file, const char __user *buf, 
 	size_t count, loff_t *pos);

void (*wake_up_page_bit_ksym)(struct page *page, int bit_nr);

bool (*page_vma_mapped_walk_ksym)(struct page_vma_mapped_walk *pvmw);

int acquire_ksyms(void)
{
	/* 
	 * Try to find all necessary symbols,
	 * return -1 if any lookup fails
	 */
	flush_tlb_mm_range_ksym = (void *)kallsyms_lookup_name("flush_tlb_mm_range");
	if(!flush_tlb_mm_range_ksym)
		return -1;

	vfs_read_ksym = (void *)kallsyms_lookup_name("vfs_read");
	if(!vfs_read_ksym)
		return -1;

	vfs_write_ksym = (void *)kallsyms_lookup_name("vfs_write");
	if(!vfs_write_ksym)
		return -1;

	wake_up_page_bit_ksym = (void *)kallsyms_lookup_name("wake_up_page_bit");
	if(!wake_up_page_bit_ksym)
		return -1;

	page_vma_mapped_walk_ksym = (void *)kallsyms_lookup_name("page_vma_mapped_walk");
	if(!page_vma_mapped_walk_ksym)
		return -1;

	return 0;
}

/* 
 * Provide the unexported kernel symbols
 * by calling the appropriate initialized
 * function pointer
 */
void flush_tlb_mm_range(struct mm_struct *mm, unsigned long start,
 	unsigned long end, unsigned long vmflag)
{
	flush_tlb_mm_range_ksym(mm, start, end, vmflag);
}

ssize_t vfs_read(struct file *file, char __user *buf, 
 	size_t count, loff_t *pos)
{
	return vfs_read_ksym(file, buf, count, pos);
}

ssize_t vfs_write(struct file *file, const char __user *buf, 
 	size_t count, loff_t *pos)
{
	return vfs_write_ksym(file, buf, count, pos);
}

void wake_up_page_bit(struct page *page, int bit_nr)
{
	wake_up_page_bit_ksym(page, bit_nr);
}

bool page_vma_mapped_walk(struct page_vma_mapped_walk *pvmw)
{
	return page_vma_mapped_walk_ksym(pvmw);
}
