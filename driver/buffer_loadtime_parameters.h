#ifndef _BUFFER_LOADTIME_PARAMETERS_H_
#define _BUFFER_LOADTIME_PARAMETERS_H_

#include <linux/device.h>

typedef struct fraction_struct
{
  int mul;
  int logb2_shift;
} fraction_t;

typedef struct buffer_loadtime_parameters_struct
{
  long mmap_buf_size; /* Size of the di-mmap buffer in pages */
  int superpage_alloc_order;
  fraction_t hot_page_fifo_alloc_ratio;
  fraction_t victim_queue_alloc_ratio;
} buf_ld_params_t;

#define DEFAULT_SUPERPAGE_ALLOC_ORDER 0
/* Allocate 1/2 of the space to the HPF */
#define HPF_ALLOC_RATIO_SHIFT 1
/* Allocate 1/8 of the space to the victim fifo */
#define VQ_ALLOC_RATIO_SHIFT 5

extern buf_ld_params_t *to_dimmap_buf_ld_params(struct class *c);
extern int update_dimmap_buf_ld_params(struct class *c);

int init_ld_params(buf_ld_params_t *ld_params);
int copy_ld_params(buf_ld_params_t *dst, buf_ld_params_t *src);
int display_ld_params(buf_ld_params_t *ld_params, char *str);
int print_ld_params(buf_ld_params_t *ld_params);
int copy_ld_param_class_attrs(struct class_attribute *global_class_attrs, int offset);

#endif /* _BUFFER_LOADTIME_PARAMETERS_H_ */
