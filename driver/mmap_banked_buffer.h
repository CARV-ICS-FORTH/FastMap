#ifndef _MMAP_BANKED_BUFFER_H_
#define _MMAP_BANKED_BUFFER_H_

#include <linux/seq_file.h>

#include "mmap_generational_fifo_buffer.h"
#include "buffer_loadtime_parameters.h"
#include "buffer_runtime_parameters.h"
#include "shared_defines.h"

struct pr_vma_data;

typedef struct banked_buffer_struct
{
  fifo_buffer_t *banks;
  buf_ld_params_t ld_params;
  buf_rt_params_t rt_params;
} banked_buffer_t;

typedef unsigned long bank_select_t;

/*******************************************************************************
 * Implement standard functions
 * Use the buffer typedef to differentiate the name of each function
 ******************************************************************************/

/* Setting up and tearing down buffers */
banked_buffer_t *init_mmap_buffer_data_banked_buffer_t(banked_buffer_t *dummy, buf_ld_params_t *ld_params, int unused);
void cleanup_mmap_buffer_data_banked_buffer_t(banked_buffer_t *buf);

/* Manage parameters */
void reset_params_banked_buffer_t(banked_buffer_t *buf);
int init_params_banked_buffer_t(banked_buffer_t *buf, buf_ld_params_t *ld_params);
void cleanup_params_banked_buffer_t(banked_buffer_t *buf);

/* Handle adding, deleting, etc pages from the buffer */
int change_page_priority_banked_buffer_t(banked_buffer_t *buf, pgoff_t physical_addr, struct pr_vma_data *pvd, uint8_t new_pri);
void hit_page_banked_buffer_t(banked_buffer_t *buf, struct tagged_page *tagged_page);

/* Query state of page in buffer */
int page_in_buffer_banked_buffer_t(banked_buffer_t *buf, struct tagged_page *tagged_page);

/* Reset or tune the buffer state */
void reset_state_banked_buffer_t(banked_buffer_t *buf);
void reset_history_banked_buffer_t(banked_buffer_t *buf);
void print_status_banked_buffer_t(banked_buffer_t *buf);
int update_runtime_parameters_banked_buffer_t(banked_buffer_t *buf, buf_rt_params_t *rt_params);
void init_specific_counters_banked_buffer_t(banked_buffer_t *buf);

#endif /* _MMAP_BANKED_BUFFER_H_ */
