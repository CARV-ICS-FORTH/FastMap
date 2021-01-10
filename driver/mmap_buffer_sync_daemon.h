#ifndef _MMAP_BUFFER_SYNC_DAEMON_H_
#define _MMAP_BUFFER_SYNC_DAEMON_H_

#include <linux/workqueue.h>

#include "mmap_buffer_daemon.h"

struct workqueue_struct *init_mmap_buffer_sync_daemon(unsigned long id);
void cleanup_mmap_buffer_sync_daemon(struct workqueue_struct **buffer_workqueue);

int issue_mmap_buffer_sync_daemon_request(struct workqueue_struct *buffer_workqueue, struct pr_vma_data *pvd, struct tagged_page **p, int len, unsigned long id);

#endif /*  _MMAP_BUFFER_SYNC_DAEMON_H_ */
