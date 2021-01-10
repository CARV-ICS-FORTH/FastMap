#include <linux/errno.h>
#include <linux/slab.h>

#include "lpfifo_non_atomic.h"

static void init(lpfifo_t *fifo_data, unsigned long size)
{
	INIT_LIST_HEAD(&fifo_data->buffer);

	fifo_data->max_size = size;
	fifo_data->size = 0;

	spin_lock_init(&fifo_data->qlock);
}

static void reset(lpfifo_t *fifo_data)
{
	INIT_LIST_HEAD(&fifo_data->buffer);

	fifo_data->size = 0;
}

static int fifo_full(lpfifo_t *fifo_data)
{ 
	return fifo_data->size > fifo_data->max_size; 
}

static struct tagged_page *dequeue(lpfifo_t *fifo_data)
{
	struct tagged_page *tagged_page = NULL;

	if(unlikely(fifo_data->size == 0))
		return NULL;

	tagged_page = list_first_entry_or_null(&fifo_data->buffer, struct tagged_page, qlist);
	if(tagged_page != NULL){
		list_del_init(&tagged_page->qlist);
		fifo_data->size--;
		atomic_set(&tagged_page->buffer_id, BI_NONE);
		return tagged_page;
	}

	DMAP_BGON(1);
	return NULL;
}

static struct tagged_page *dequeue_if(lpfifo_t *fifo_data, int (*cond)(struct tagged_page *p))
{
	struct tagged_page *tagged_page = NULL;

	if(unlikely(fifo_data->size == 0))
		return NULL;

	tagged_page = list_first_entry_or_null(&fifo_data->buffer, struct tagged_page, qlist);
	if(tagged_page != NULL){
		DMAP_BGON(atomic_read(&tagged_page->buffer_id) != BI_CLEAN);
		if(cond(tagged_page)){ 
			list_del_init(&tagged_page->qlist);
			fifo_data->size--;
			atomic_set(&tagged_page->buffer_id, BI_NONE);
			return tagged_page;
		}else{ /* there is a page here, but we cannot dequeue it. So we move it to the tail. */
			list_del_init(&tagged_page->qlist);
			list_add_tail(&tagged_page->qlist, &fifo_data->buffer);
			DMAP_BGON(atomic_read(&tagged_page->buffer_id) != BI_CLEAN);
		}
	}

	return NULL;
}

static int enqueue(lpfifo_t *fifo_data, struct tagged_page *tagged_page)
{
	if(fifo_data->size > fifo_data->max_size)
		return -1;

	DMAP_BGON(tagged_page == NULL);
	DMAP_BGON(fifo_data == NULL);

	list_add_tail(&tagged_page->qlist, &fifo_data->buffer);
	fifo_data->size++;

	atomic_set(&tagged_page->buffer_id, BI_CLEAN);

	return 0;
}

static void remove(lpfifo_t *fifo_data, struct tagged_page *tagged_page)
{
	DMAP_BGON(atomic_read(&tagged_page->buffer_id) != BI_CLEAN);
	list_del_init(&tagged_page->qlist);
	fifo_data->size--;
	atomic_set(&tagged_page->buffer_id, BI_NONE);
}

static void change_priority(lpfifo_t *fifo_data, uint8_t new_pri, struct tagged_page *tagged_page)
{
	BUG();
}

static unsigned long size(lpfifo_t *fifo_data)
{
	return fifo_data->size;
}

struct lpfifo_operations_struct lpfifo_operations = {
	.init = init,
	.reset = reset,
	.fifo_full = fifo_full,
	.enqueue = enqueue,
	.dequeue = dequeue,
	.dequeue_if = dequeue_if,
	.remove = remove,
	.change_priority = change_priority,
	.size = size,
};
