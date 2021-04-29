#include <linux/mm.h> /* everything */
#include <linux/sched.h>
#include <linux/mempolicy.h>
#include <asm/pgtable.h>
#include "mmap_banked_buffer.h"
#include "tagged_page.h"
#include "mmap_buffer_interface.h"
#include "btree_metadata.h"
#include "mmap_generational_fifo_buffer.h"
#include "shared_defines.h"
#include "dmap.h"

banked_buffer_t *init_mmap_buffer_data_banked_buffer_t(banked_buffer_t *dummy, buf_ld_params_t *ld_params, int unused)
{
	int i = 0;
	banked_buffer_t *buf;
#ifdef CONFIG_NUMA
	struct mempolicy *pol = current->mempolicy;
#endif
	buf_ld_params_t bank_ld_params;

#ifdef CONFIG_NUMA
	if(pol == NULL){
		printk(KERN_ALERT "DMAP WARNING: NUMA allocation policy is set to system default\n");
	}else{
		if(pol->mode != MPOL_INTERLEAVE){
			char *tmp;
			switch (pol->mode)
			{
				case MPOL_PREFERRED:
					tmp = "PREFERRED";
					break;
				case MPOL_INTERLEAVE:
					tmp = "INTERLEAVE";
					break;
				case MPOL_DEFAULT:
					tmp = "DEFAULT";
					break;
				case MPOL_BIND:
					tmp = "BIND";
					break;
				default:
					tmp = "UNKNOWN";
			}
			printk(KERN_ALERT "DMMAP WARNING: NUMA allocation policy is not set to INTERLEAVE it is %s(%d)\n", tmp, pol->mode);
		}else{
			printk(KERN_ALERT "DMMAP WARNING: NUMA allocation policy is set to INTERLEAVE\n");
		}
	}
#endif

	buf = (banked_buffer_t *)kmalloc(sizeof(banked_buffer_t), GFP_KERNEL | __GFP_ZERO);
	DMAP_BGON(buf == NULL);

	memset(buf, 0, sizeof(banked_buffer_t));

	/* Copy in the loadtime parameters from the main di-mmap structure */
	init_ld_params(&buf->ld_params);
	copy_ld_params(&buf->ld_params, ld_params);
	buf->banks = (fifo_buffer_t *)kmalloc(sizeof(fifo_buffer_t *), GFP_KERNEL | __GFP_ZERO);
	DMAP_BGON(buf->banks == NULL);

	/* Copy the loadtime parameters down into the individual banks, and tweak as necessary */
	copy_ld_params(&bank_ld_params, ld_params);
	bank_ld_params.mmap_buf_size = ld_params->mmap_buf_size;

	init_rt_params(&buf->rt_params);

	/* Do not try to acquire the bank locks here because they have not been initialized */
	buf->banks = init_mmap_buffer_data(buf->banks, &bank_ld_params, i);

	return buf;
}

void cleanup_mmap_buffer_data_banked_buffer_t(banked_buffer_t *buf)
{
	if(buf == NULL){
		printk(KERN_INFO "Data structure for the banked buffer was never initialized, nothing to cleanup\n");
		return;
	}

	printk(KERN_INFO "Cleaning up data structures for the banked buffer\n");
	cleanup_mmap_buffer_data(buf->banks);
	kfree(buf);
}

void reset_params_banked_buffer_t(banked_buffer_t *buf)
{
	spin_lock(&buf->banks->buffer_lock);
	reset_params(buf->banks);
	spin_unlock(&buf->banks->buffer_lock);
}

int init_params_banked_buffer_t(banked_buffer_t *buf, buf_ld_params_t *ld_params)
{
	int err = 0;
	buf_ld_params_t bank_ld_params;

	DMAP_BGON(ld_params == NULL);
	copy_ld_params(&buf->ld_params, ld_params);
	copy_ld_params(&bank_ld_params, ld_params);
	bank_ld_params.mmap_buf_size = ld_params->mmap_buf_size;

	/* Allocate basic data structures */
	/* FIXME[apapag]: I don't think we need the lock here. Lockstat complains. */
	//spin_lock(&buf->banks->buffer_lock);
	err = init_params(buf->banks, &bank_ld_params);
	//spin_unlock(&buf->banks->buffer_lock);

	return err;
}

void cleanup_params_banked_buffer_t(banked_buffer_t *buf)
{
	if(buf == NULL){
		printk(KERN_INFO "Data structure for the banked buffer was never initialized, no parameters to cleanup\n");
		return;
	}

	spin_lock(&buf->banks->buffer_lock);
	cleanup_params(buf->banks);
	spin_unlock(&buf->banks->buffer_lock);
}

int change_page_priority_banked_buffer_t(banked_buffer_t *buf, pgoff_t physical_addr, struct pr_vma_data *pvd, uint8_t new_pri)
{
	int ret;

	spin_lock(&buf->banks->buffer_lock);
	ret = change_page_priority(buf->banks, physical_addr, pvd, new_pri);
	spin_unlock(&buf->banks->buffer_lock);

	return ret;
}

void hit_page_banked_buffer_t(banked_buffer_t *buf, struct tagged_page *tagged_page)
{
	hit_page(buf->banks, tagged_page);
}

/* Query state of page in buffer */
int page_in_buffer_banked_buffer_t(banked_buffer_t *buf, struct tagged_page *tagged_page)
{
	return page_in_buffer(buf->banks, tagged_page);
}

/* Reset or tune the buffer state */
void reset_state_banked_buffer_t(banked_buffer_t *buf)
{
	spin_lock(&buf->banks->buffer_lock);
	reset_state(buf->banks);
	spin_unlock(&buf->banks->buffer_lock);
}

/* Reset the buffer fault history */
void reset_history_banked_buffer_t(banked_buffer_t *buf)
{
	spin_lock(&buf->banks->buffer_lock);
	reset_history(buf->banks);
	spin_unlock(&buf->banks->buffer_lock);
}

int update_runtime_parameters_banked_buffer_t(banked_buffer_t *buf, buf_rt_params_t *rt_params)
{
	int err = 0;

	if(rt_params != NULL)
		copy_rt_params(&buf->rt_params, rt_params);

	spin_lock(&buf->banks->buffer_lock);
	err = update_runtime_parameters(buf->banks, &buf->rt_params);
	spin_unlock(&buf->banks->buffer_lock);

	return err;
}

void init_specific_counters_banked_buffer_t(banked_buffer_t *buf)
{
	spin_lock(&buf->banks->buffer_lock);
	init_specific_counters(buf->banks);
	spin_unlock(&buf->banks->buffer_lock);
}

void print_status_banked_buffer_t(banked_buffer_t *buf){}
