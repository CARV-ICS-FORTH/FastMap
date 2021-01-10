#ifndef _MMAP_BUFFER_DAEMON_H_
#define _MMAP_BUFFER_DAEMON_H_

#include <linux/workqueue.h>

typedef struct buffer_workqueue_request_t 
{
	struct work_struct q_work;
	struct pr_vma_data *pvd;
	struct tagged_page **p;
	int len;
	int id;

} mmap_buffer_workqueue_request_t;

#endif /*  _MMAP_BUFFER_DAEMON_H_ */
