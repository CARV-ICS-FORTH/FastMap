#include "dmap.h"
#include <linux/delay.h>

static ssize_t dimmap_show(struct class *class, struct class_attribute *attr, char *buf, ssize_t (*format)(dimmap_buffer_t *dimmap_buf, char *))
{
  dimmap_buffer_t *dimmap_buf = to_dimmap_buf(class);
  ssize_t ret = -EINVAL;
  ret = (*format)(dimmap_buf, buf);
  return ret;
}

/*************************************************************************
 * System runtime parameters
 ************************************************************************/
/* Generate a show function for a simple load-time global field */
#define SHOW_SYS_RT_PARAM(field, format_string)                                             \
  static ssize_t sys_rt_format_##field(dimmap_buffer_t *dimmap_buf, char *buf)              \
  {                                                                                         \
    return scnprintf(buf, PAGE_SIZE, format_string, dimmap_buf->sys_rt_params.field);       \
  }                                                                                         \
  ssize_t show_sys_rt_##field(struct class *class, struct class_attribute *attr, char *buf) \
  {                                                                                         \
    return dimmap_show(class, attr, buf, sys_rt_format_##field);                            \
  }

SHOW_SYS_RT_PARAM(buffer_state, "%d\n")
ssize_t store_sys_rt_buffer_state(struct class *class, struct class_attribute *attr, const char *buf, size_t count)
{
  dimmap_buffer_t *dimmap_buf = to_dimmap_buf(class);
  int ret = -EINVAL;
  int val;

  ret = kstrtoint(buf, 0, &val);
  if (ret)
  {
    goto err;
  }
  ret = count;

  printk(KERN_ERR "%s:%s:%d\n", __FILE__, __func__, __LINE__);

  /* Note that the buffer_state field is set by the allocate and deallocate routines */
  switch (val)
  {
  case 0: /* Unload / deallocate buffer */

    printk(KERN_ERR "%s:%s:%d Deallocating buffer...\n", __FILE__, __func__, __LINE__);
    if (dimmap_buf->sys_rt_params.buffer_state != 0)
    {
      /* Only deallocate an allocated buffer */
      deallocate_runtime_buffer(dimmap_buf);
    }
    printk(KERN_ERR "%s:%s:%d Deallocating buffer...DONE!\n", __FILE__, __func__, __LINE__);
    break;
  case 1: /* Load / allocate buffer */
    if (dimmap_buf->sys_rt_params.buffer_state != 0)
    {
      /* Deallocate the buffer if it is already allocated. */
      deallocate_runtime_buffer(dimmap_buf);
    }
    allocate_and_init_runtime_buffer(dimmap_buf);
    break;
  default:
    WARN(1, "WARNING DI-MMAP Parameters: Invalid parameter passed to write-back daemon state %d\n", val);
  }

  printk(KERN_ERR "%s:%s:%d\n", __FILE__, __func__, __LINE__);

err:
  return ret;
}

/* Generate a store function for an integer runtime parameter field */
#define TOGGLE_INTEGER_SYS_RT_PARAM(field, fn, test, reset)                                                             \
  ssize_t toggle_sys_rt_param_##field(struct class *class, struct class_attribute *attr, const char *buf, size_t count) \
  {                                                                                                                     \
    dimmap_buffer_t *dimmap_buf = to_dimmap_buf(class);                                                                 \
    int ret = -EINVAL;                                                                                                  \
    int val = 0;                                                                                                        \
    ret = kstrtoint(buf, 0, &val);                                                                                      \
    if (ret)                                                                                                            \
    {                                                                                                                   \
      goto err;                                                                                                         \
    }                                                                                                                   \
    dimmap_buf->sys_rt_params.field = val;                                                                              \
    if (dimmap_buf->sys_rt_params.field == test)                                                                        \
    {                                                                                                                   \
      fn(class);                                                                                                        \
      dimmap_buf->sys_rt_params.field = reset;                                                                          \
    }                                                                                                                   \
    ret = count;                                                                                                        \
  err:                                                                                                                  \
    return ret;                                                                                                         \
  }

SHOW_SYS_RT_PARAM(reset_buffer_fields, "%d\n")
void reset_buffer_fields_fn(struct class *class)
{
  printk(KERN_INFO "DI-MMAP: Reseting the mmap fields in the buffers\n");
  reset_mmap_state();
  printk(KERN_INFO "DI-MMAP: Finished reseting the mmap fields in the buffers\n");
}
TOGGLE_INTEGER_SYS_RT_PARAM(reset_buffer_fields, reset_buffer_fields_fn, 1, 0);

SHOW_SYS_RT_PARAM(scan_ptes, "%d\n")
void scan_ptes_fn(struct class *class)
{
  printk(KERN_INFO "DI-MMAP: Start scanning the Page Table Entries (PTEs)\n");
#if 0
  for(i = 0; i < MAX_DI_MMAP_MINORS; i++) {
    if(raw_devices[i].inuse) {
/*       scan_page_tables(&raw_devices[i].active_vmas); */
    }
  }
#endif
  printk(KERN_INFO "UNIMPLEMENTED FEATURE\n");
  printk(KERN_INFO "DI-MMAP: Finished scanning the Page Table Entries (PTEs)\n");
}
TOGGLE_INTEGER_SYS_RT_PARAM(scan_ptes, scan_ptes_fn, 1, 0);

SHOW_SYS_RT_PARAM(debug_lvl, "%d\n")
ssize_t store_sys_rt_debug_lvl(struct class *class, struct class_attribute *attr, const char *buf, size_t count)
{
  dimmap_buffer_t *dimmap_buf = to_dimmap_buf(class);
  int ret = -EINVAL;
  int val;

  ret = kstrtoint(buf, 0, &val);
  if (ret)
  {
    goto err;
  }
  ret = count;

  dimmap_buf->sys_rt_params.debug_lvl = val;

  if (dimmap_buf->sys_rt_params.debug_lvl >= 1)
  {
    printk("Enabled debugging mode -- logging level %d\n", dimmap_buf->sys_rt_params.debug_lvl);
  }
  else
  {
    printk("Disabled debugging mode\n");
  }

err:
  return ret;
}

SHOW_SYS_RT_PARAM(reset_device_stats, "%d\n")
void reset_device_stats_fn(struct class *class)
{
  dimmap_buffer_t *dimmap_buf = to_dimmap_buf(class);
  if (dimmap_buf->sys_rt_params.buffer_state != 0)
  {
    //reset_device_parameters(); // XXX
    printk("Resetting device parameters to defaults\n");
    printk("Clearing device counters\n");
  }
  else
  {
    printk("Unable to resetting device parameters, buffer not loaded\n");
  }
}
TOGGLE_INTEGER_SYS_RT_PARAM(reset_device_stats, reset_device_stats_fn, 1, 0);

SHOW_SYS_RT_PARAM(clear_counters, "%d\n")
void clear_counters_fn(struct class *class)
{
  dimmap_buffer_t *dimmap_buf = to_dimmap_buf(class);
  if (dimmap_buf->sys_rt_params.buffer_state != 0)
  {
    printk("Clearing device counters\n");
  }
  else
  {
    printk("Unable to clear device counters, buffer not loaded\n");
  }
}
TOGGLE_INTEGER_SYS_RT_PARAM(clear_counters, clear_counters_fn, 1, 0);

void init_sys_rt_params(sys_rt_params_t *sys_rt_params)
{
  sys_rt_params->buffer_state = 0;
  sys_rt_params->reset_buffer_fields = 0;
  sys_rt_params->scan_ptes = 0;
  sys_rt_params->debug_lvl = 0;
  sys_rt_params->reset_device_stats = 0;
  sys_rt_params->clear_counters = 0;
}

/*************************************************************************
 * Buffer runtime parameters
 ************************************************************************/

/*************************************************************************
 * Buffer loadtime parameters
 ************************************************************************/

/* generate a show function for simple field */
#define DIMMAP_SHOW(field, format_string)                                                   \
  static ssize_t dimmap_format_##field(dimmap_buffer_t *dimmap_buf, char *buf)              \
  {                                                                                         \
    return scnprintf(buf, PAGE_SIZE, format_string, dimmap_buf->buf_data->field);           \
  }                                                                                         \
  ssize_t dimmap_show_##field(struct class *class, struct class_attribute *attr, char *buf) \
  {                                                                                         \
    return dimmap_show(class, attr, buf, dimmap_format_##field);                            \
  }

/* System runtime parameters */
static struct class_attribute sys_rt_param_class_attrs[] = {
    __ATTR(0_buffer_state, S_IRUGO | S_IWUSR, show_sys_rt_buffer_state, store_sys_rt_buffer_state),
    __ATTR(0_reset_buffer_fields, S_IRUGO | S_IWUSR, show_sys_rt_reset_buffer_fields, toggle_sys_rt_param_reset_buffer_fields),
    __ATTR(0_scan_PTEs, S_IRUGO | S_IWUSR, show_sys_rt_scan_ptes, toggle_sys_rt_param_scan_ptes),
    __ATTR(0_debug_lvl, S_IRUGO | S_IWUSR, show_sys_rt_debug_lvl, store_sys_rt_debug_lvl),
    __ATTR(0_reset_device_stats, S_IRUGO | S_IWUSR, show_sys_rt_reset_device_stats, toggle_sys_rt_param_reset_device_stats),
    __ATTR(0_clear_counters, S_IRUGO | S_IWUSR, show_sys_rt_clear_counters, toggle_sys_rt_param_clear_counters),
    __ATTR_NULL,
};

int copy_sys_rt_param_class_attrs(struct class_attribute *global_class_attrs, int offset)
{
  int i = 0;
  while (sys_rt_param_class_attrs[i].attr.name != NULL)
  {
    global_class_attrs[offset + i] = sys_rt_param_class_attrs[i];
    i++;
  }
  return offset + i;
}
