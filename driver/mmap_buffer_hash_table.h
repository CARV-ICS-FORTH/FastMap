#ifndef _MMAP_BUFFER_HASH_TABLE_H_
#define _MMAP_BUFFER_HASH_TABLE_H_

#include <linux/list.h>
#include <linux/hashtable.h>

#include "tagged_page.h"
#include "shared_defines.h"

struct pr_vma_data;

typedef struct buffer_map_struct
{
#if NUM_FREELISTS == 1
	struct list_head fl;
	spinlock_t fl_lock;
#else
	struct list_head *fl;
	spinlock_t *fl_lock;
#endif

	struct tagged_page *pages;
	long total_num_pages;
	void *owner; // (fifo_buffer_t *)
} buffer_map_t;

void init_hash_table_free_pages(buffer_map_t *page_map, long num_free_pages);
void free_hash_table(buffer_map_t *page_map);

struct tagged_page *alloc_page_lock(buffer_map_t *page_map, pgoff_t page_offset, struct pr_vma_data *pvd);
void free_page_lock(buffer_map_t *page_map, struct tagged_page *tagged_page);

#endif /* _MMAP_BUFFER_HASH_TABLE_H_ */
