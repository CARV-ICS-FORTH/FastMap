#ifndef _DI_MMAP_PARAMETERS_H_
#define _DI_MMAP_PARAMETERS_H_

typedef struct sys_rt_parameters_struct
{
  int buffer_state;        /* Is the di-mmap buffer allocated / loaded: 1 - loaded, 0 - unloaded */
  int reset_buffer_fields; /* Reset the buffer counters, etc. */
  int scan_ptes;
  int debug_lvl;
  int reset_device_stats;
  int clear_counters;
} sys_rt_params_t;

/*************************************************************************
 * System runtime parameters
 ************************************************************************/
void init_sys_rt_params(sys_rt_params_t *sys_rt_params);
int copy_sys_rt_param_class_attrs(struct class_attribute *global_class_attrs, int offset);

/*************************************************************************
 * System loadtime parameters
 ************************************************************************/
int initialize_dimmap_parameters(struct class *class);
void cleanup_dimmap_parameters(struct class *class);

#endif /* _DI_MMAP_PARAMETERS_H_ */
