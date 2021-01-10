#ifndef _LPFIFO_H_
#define _LPFIFO_H_

#include "tagged_page.h"

typedef struct lpfifo_struct
{
	struct list_head buffer;

	unsigned long size;
	unsigned long max_size;

	spinlock_t qlock;
} lpfifo_t;

struct lpfifo_operations_struct
{
	void (*init)(lpfifo_t *fifo_data, unsigned long size);
	void (*reset)(lpfifo_t *fifo_data);
	int (*fifo_full)(lpfifo_t *fifo_data);
	int (*enqueue)(lpfifo_t *fifo_data, struct tagged_page *tagged_page);
	struct tagged_page *(*dequeue)(lpfifo_t *fifo_data);
	struct tagged_page *(*dequeue_if)(lpfifo_t *fifo_data, int (*cond)(struct tagged_page *));
	void (*remove)(lpfifo_t *fifo_data, struct tagged_page *tagged_page);
	void (*change_priority)(lpfifo_t *fifo_data, uint8_t new_pri, struct tagged_page *tagged_page);
	unsigned long (*size)(lpfifo_t *fifo_data);
};

extern struct lpfifo_operations_struct lpfifo_operations;

#endif // _LPFIFO_H_
