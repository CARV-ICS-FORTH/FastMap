#include <linux/kernel.h>	/* printk() */
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/delay.h>

#include "dmap.h"
#include "mmap_buffer_sync_daemon.h"

extern void write_file_async(struct tagged_page **p, int len);
extern void write_bio_async(struct tagged_page **p, int len);

/* Use the workqueue to incrementally sync the buffers in the background */
static void workqueue_sync_buffers(struct work_struct *work)
{
	/* Extract the structure that contains the work_struct field */
	mmap_buffer_workqueue_request_t *data = container_of(work, mmap_buffer_workqueue_request_t, q_work);

	struct pr_vma_data *pvd = data->pvd;
	struct tagged_page **p = data->p;
	int len = data->len;

	kfree(data); /* Free the message container */

	//printk(KERN_ERR "Calling write async with len = %d!\n", len);

	if(pvd->type == D_BLKD)
		write_bio_async(p, len);
	else /* pvr->type == D_FILE */
		write_file_async(p, len);

	/*
	 * This page allocated by the caller of 
	 * issue_mmap_buffer_sync_daemon_request()
	 * and we have to deallocate it.
	 */
	free_page((unsigned long)p);
}

/* Issue a request to clear the victim fifo */
int issue_mmap_buffer_sync_daemon_request(struct workqueue_struct *buffer_workqueue, struct pr_vma_data *pvd, struct tagged_page **p, int len, unsigned long id)
{
	mmap_buffer_workqueue_request_t *req;
	int ret;

	req = (mmap_buffer_workqueue_request_t *)kmalloc(sizeof(mmap_buffer_workqueue_request_t), GFP_KERNEL);
	DMAP_BGON(req == NULL);

	INIT_WORK(&req->q_work, &workqueue_sync_buffers);

	req->pvd = pvd;;
	req->p = p;
	req->len = len;
	req->id = id;

	//if(!work_pending(&req->q_work))
	ret = queue_work(buffer_workqueue, &req->q_work);

	return ret;
}

struct workqueue_struct *init_mmap_buffer_sync_daemon(unsigned long id)
{
	struct workqueue_struct *buffer_workqueue;
	char queue_name[128];
	
//	printk(KERN_ERR "Initializing workqueue %ld\n", id);

	sprintf(queue_name, "fmap-sync%2ld", -1-id);
	buffer_workqueue = create_singlethread_workqueue(queue_name);
	
	if(id == 0)
		printk(KERN_ALERT "init_mmap_buffer_sync_daemon\n");

	return buffer_workqueue;
}

void cleanup_mmap_buffer_sync_daemon(struct workqueue_struct **buffer_workqueue)
{
	if(*buffer_workqueue != NULL){
		flush_workqueue(*buffer_workqueue);
		destroy_workqueue(*buffer_workqueue);
		*buffer_workqueue = NULL;
	}
}
