#ifndef _MMAP_FAULT_HANDLER_H_
#define _MMAP_FAULT_HANDLER_H_

#include "tagged_page.h"
#include "helpers.h"
#include "mmap_buffer_interface.h"
#include "buffer_loadtime_parameters.h"
#include "buffer_runtime_parameters.h"

extern struct buffer_operations_struct *buf_ops;
extern banked_buffer_t *buf_data;
typedef banked_buffer_t mmap_buffer_t;

struct pr_vma_data; // forward declaration

/* Function prototypes */
int reset_mmap_params(buf_ld_params_t *ld_params);
void reset_mmap_state(void);
void reset_mmap_history(void);
void print_status_dimmap_buffer(void);
void reset_stats_dimmap_buffer(void);
int init_mmap_params(buf_ld_params_t *ld_params);
void cleanup_mmap_params(void);
int update_mmap_loadtime_parameters(mmap_buffer_t *buf, buf_ld_params_t *ld_params);
int update_mmap_runtime_parameters(mmap_buffer_t *buf, buf_rt_params_t *rt_params);

#endif /* _MMAP_FAULT_HANDLER_H_ */
