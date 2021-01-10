#ifndef _PERMA_MMAP_GENERATIONAL_FIFO_BUFFER_H_
#define _PERMA_MMAP_GENERATIONAL_FIFO_BUFFER_H_

#include <linux/seq_file.h>
#include <linux/rtmutex.h>

#include "mmap_buffer_hash_table.h"

#include "lpfifo_non_atomic.h"
#include "dfifo_non_atomic.h"

#include "buffer_loadtime_parameters.h"
#include "buffer_runtime_parameters.h"

#include "shared_defines.h"

#define MAX_PRIORITIES 16

#define PRIMARY_T 0x00
#define VICTIM_T  0x03

struct pr_vma_data;

typedef struct fifo_buffer_struct 
{
	/*
	 * Each allocated page has to be in *exactly* one of the 2
	 * following queues. The primary_fifo_data queue contains 
	 * only clean pages and it also applies priorities. In the
	 * case of a page fault and if there is not a free page,
	 * the page fault thread evicts some pages from this queue.
	 * We never evict dirty pages as it also increases the 
	 * tail latency due to the required I/O. The dirty_queue
	 * contains only dirty pages. This does not apply priorities
	 * and a separate thread checks if a specified threshold 
	 * exceeds and writeback some pages and the move them in
	 * clean queue.
	 */
	lpfifo_t *primary_fifo_data[NUM_QUEUES]; 

	/*
	 * We have one dirty_queue per evictor thread.
	 * This enables more fine grain locking when removing 
	 * items from the dirty queue. Enqueuing in clean queue
	 * is not an issues as we can add batches of pages to
	 * it.
	 */
	dfifo_t *dirty_queue[EVICTOR_THREADS];	

	buf_ld_params_t ld_params;
	buf_rt_params_t rt_params;

	unsigned long primary_fifo_buffer_size;

	buffer_map_t page_map;

	spinlock_t buffer_lock;

	bool evictor_run;
	struct task_struct *evictor[EVICTOR_THREADS];

} fifo_buffer_t;

#define MAX_NUM_SKIPPED_FIFO_ENTRIES 16

//extern struct buffer_operations_struct fifo_buffer_ops;
extern fifo_buffer_t fifo_buffer_data;

/*******************************************************************************
 * Implement standard functions
 * Use the buffer typedef to differentiate the name of each function
 ******************************************************************************/

/* Setting up and tearing down buffers */
fifo_buffer_t *init_mmap_buffer_data_fifo_buffer_t(fifo_buffer_t *dummy, buf_ld_params_t *ld_params, int bank_id);
void cleanup_mmap_buffer_data_fifo_buffer_t(fifo_buffer_t *buf);

/* Manage parameters */
void reset_params_fifo_buffer_t(fifo_buffer_t *buf);
int init_params_fifo_buffer_t(fifo_buffer_t *buf, buf_ld_params_t *ld_params);
void cleanup_params_fifo_buffer_t(fifo_buffer_t *buf);

/* Handle adding, deleting, etc pages from the buffer */
int change_page_priority_fifo_buffer_t(fifo_buffer_t *buf, pgoff_t physical_addr, struct pr_vma_data *pvd, uint8_t new_pri);
void insert_page_fifo_buffer_t(fifo_buffer_t *buf, struct tagged_page *tagged_page, bool clear);
void hit_page_fifo_buffer_t(fifo_buffer_t *buf, struct tagged_page *tagged_page);

/* Query state of page in buffer */
int page_in_buffer_fifo_buffer_t(fifo_buffer_t *buf, struct tagged_page *tagged_page);

/* Reset or tune the buffer state */
void reset_state_fifo_buffer_t(fifo_buffer_t *buf);
void reset_history_fifo_buffer_t(fifo_buffer_t *buf);
void print_status_fifo_buffer_t(fifo_buffer_t *buf);
int update_runtime_parameters_fifo_buffer_t(fifo_buffer_t *buf, buf_rt_params_t *rt_params);
void init_specific_counters_fifo_buffer_t(fifo_buffer_t *buf);

void move_page_c2d_fifo_buffer_t(fifo_buffer_t *buf, struct tagged_page *tagged_page);
void move_page_d2c_fifo_buffer_t(fifo_buffer_t *buf, struct tagged_page *tagged_page);

#endif /* _PERMA_MMAP_GENERATIONAL_FIFO_BUFFER_H_ */
