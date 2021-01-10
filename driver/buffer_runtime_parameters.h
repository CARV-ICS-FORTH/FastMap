#ifndef _BUFFER_RUNTIME_PARAMETERS_H_
#define _BUFFER_RUNTIME_PARAMETERS_H_

#include <linux/device.h>

typedef struct buffer_runtime_parameters_struct
{
  int dirty_page_hotness;
  int min_free_pages_per_bank;
  int log_fault_sequence;
  int fault_sequence_tag;
  int reset_history;
  int print_status;
  int reset_stats;
  int drop_invalidated_pages;
} buf_rt_params_t;

#define DIRTY_PAGE_HOTNESS 0
#define MIN_FREE_PAGES_PER_BANK 8

extern buf_rt_params_t *to_dimmap_buf_rt_params(struct class *c);
extern int update_dimmap_buf_rt_params(struct class *c);

extern void reset_mmap_history(void);
extern void print_status_dimmap_buffer(void);
extern void reset_stats_dimmap_buffer(void);

int init_rt_params(buf_rt_params_t *rt_params);
int copy_rt_params(buf_rt_params_t *dst, buf_rt_params_t *src);
int display_rt_params(buf_rt_params_t *rt_params, char *str);
int print_rt_params(buf_rt_params_t *rt_params);
int copy_rt_param_class_attrs(struct class_attribute *global_class_attrs, int offset);

#endif /* _BUFFER_RUNTIME_PARAMETERS_H_ */
