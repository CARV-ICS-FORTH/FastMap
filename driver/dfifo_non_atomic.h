#ifndef _DFIFO_H_
#define _DFIFO_H_

#include "tagged_page.h"

typedef struct dfifo_struct
{
	struct list_head buffer;

	long size, max_size;
	
	spinlock_t dlock;

} dfifo_t;

struct dfifo_operations_struct
{
	void (*init)(dfifo_t *fifo_data, unsigned long size);
	void (*reset)(dfifo_t *fifo_data);
	int (*fifo_full)(dfifo_t *fifo_data);
	struct tagged_page *(*front)(dfifo_t *fifo_data);
	struct tagged_page *(*dequeue)(dfifo_t *fifo_data);
	struct tagged_page *(*dequeue_if)(dfifo_t *fifo_data, int (*cond)(struct tagged_page *));
	int (*enqueue)(dfifo_t *fifo_data, struct tagged_page *tagged_page);
	void (*remove)(dfifo_t *fifo_data, struct tagged_page *tagged_page);
	int (*remove_if)(dfifo_t *fifo_data, struct tagged_page *tagged_page, int (*cond)(struct tagged_page *));
};

extern struct dfifo_operations_struct dfifo_operations;

#endif // _DFIFO_H_
