#include <linux/errno.h>
#include <linux/slab.h>

#include "dfifo_non_atomic.h"

static void init(dfifo_t *fifo_data, unsigned long size)
{
	INIT_LIST_HEAD(&fifo_data->buffer);

	fifo_data->max_size = size;
	fifo_data->size = 0;

	spin_lock_init(&fifo_data->dlock);
}

static void reset(dfifo_t *fifo_data)
{
	INIT_LIST_HEAD(&fifo_data->buffer);

	fifo_data->size = 0;
}

static int fifo_full(dfifo_t *fifo_data)
{ 
	return fifo_data->size > fifo_data->max_size; 
}

static struct tagged_page *front(dfifo_t *fifo_data)
{
	return list_first_entry_or_null(&fifo_data->buffer, struct tagged_page, qlist);
}

static struct tagged_page *dequeue(dfifo_t *fifo_data)
{
	struct tagged_page *tagged_page = NULL;

	if (unlikely(fifo_data->size == 0))
		return NULL;

	tagged_page = list_first_entry_or_null(&fifo_data->buffer, struct tagged_page, qlist);
	if(tagged_page != NULL){
		list_del_init(&tagged_page->qlist);
		fifo_data->size--;
		atomic_set(&tagged_page->buffer_id, BI_NONE);
		return tagged_page;
	}

	return NULL;
}

static struct tagged_page *dequeue_if(dfifo_t *fifo_data, int (*cond)(struct tagged_page *))
{
	struct tagged_page *tagged_page = NULL;

	if (unlikely(fifo_data->size == 0))
		return NULL;

	tagged_page = list_first_entry_or_null(&fifo_data->buffer, struct tagged_page, qlist);
	if(tagged_page != NULL){
		if(cond(tagged_page)){
			DMAP_BGON(atomic_read(&tagged_page->buffer_id) != BI_DIRTY);
			list_del_init(&tagged_page->qlist);
			fifo_data->size--;
			atomic_set(&tagged_page->buffer_id, BI_NONE);
			return tagged_page;
		}else{ /* there is a page here, but we cannot dequeue it. So we move it to the tail. */
			DMAP_BGON(atomic_read(&tagged_page->buffer_id) != BI_DIRTY);
			list_del_init(&tagged_page->qlist);
			list_add_tail(&tagged_page->qlist, &fifo_data->buffer);
			DMAP_BGON(atomic_read(&tagged_page->buffer_id) != BI_DIRTY);
		}
	}

	return NULL;
}

static int enqueue(dfifo_t *fifo_data, struct tagged_page *tagged_page)
{
	if (fifo_data->size > fifo_data->max_size)
		return -1;

	DMAP_BGON(tagged_page == NULL);
	DMAP_BGON(fifo_data == NULL);

	list_add_tail(&tagged_page->qlist, &fifo_data->buffer);
	fifo_data->size++;
	atomic_set(&tagged_page->buffer_id, BI_DIRTY);

	return 0;
}

static void remove(dfifo_t *fifo_data, struct tagged_page *tagged_page)
{
	DMAP_BGON(atomic_read(&tagged_page->buffer_id) != BI_DIRTY);
	list_del_init(&tagged_page->qlist);
	fifo_data->size--;
	atomic_set(&tagged_page->buffer_id, BI_NONE);
}

static int remove_if(dfifo_t *fifo_data, struct tagged_page *tagged_page, int (*cond)(struct tagged_page *))
{
	DMAP_BGON(tagged_page == NULL);
	if(tagged_page != NULL && cond(tagged_page)){	
		DMAP_BGON(atomic_read(&tagged_page->buffer_id) != BI_DIRTY);
		list_del_init(&tagged_page->qlist);
		fifo_data->size--;
		atomic_set(&tagged_page->buffer_id, BI_NONE);
		return 1;
	}
	return 0;
}

struct dfifo_operations_struct dfifo_operations = {
	.init = init,
	.reset = reset,
	.fifo_full = fifo_full,
	.front = front,
	.enqueue = enqueue,
	.dequeue = dequeue,
	.dequeue_if = dequeue_if,
	.remove = remove,
	.remove_if = remove_if,
};
