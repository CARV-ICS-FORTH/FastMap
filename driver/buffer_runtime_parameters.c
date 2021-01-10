#include <linux/mm.h>

#include <linux/module.h>
#include <linux/mm.h>    /* everything */
#include <linux/errno.h> /* error codes */
#include <asm/pgtable.h>
#include <linux/delay.h>
#include <linux/time.h>
#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/blkdev.h>

#include "dmap.h"
#include "mmap_fault_handler.h"
#include "mmap_buffer_interface.h"

#include "buffer_runtime_parameters.h"

extern banked_buffer_t *buf_data;
extern atomic64_t ms_in_open;
extern atomic64_t ms_in_fsync;
extern atomic64_t ms_in_get_page;

extern void ino_cache_clear(void);

int init_rt_params(buf_rt_params_t *rt_params)
{
  rt_params->dirty_page_hotness = DIRTY_PAGE_HOTNESS;
  rt_params->min_free_pages_per_bank = MIN_FREE_PAGES_PER_BANK;
  rt_params->log_fault_sequence = 1; /* Enable logging by default */
  rt_params->fault_sequence_tag = 0;
  rt_params->reset_history = 0;
  rt_params->print_status = 0;
  rt_params->reset_stats = 0;
  rt_params->drop_invalidated_pages = 0;
  return 0;
}

int copy_rt_params(buf_rt_params_t *dst, buf_rt_params_t *src)
{
  dst->dirty_page_hotness = src->dirty_page_hotness;
  dst->min_free_pages_per_bank = src->min_free_pages_per_bank;
  dst->log_fault_sequence = src->log_fault_sequence;
  dst->fault_sequence_tag = src->fault_sequence_tag;
  dst->reset_history = src->reset_history;
  dst->print_status = src->print_status;
  dst->reset_stats = src->reset_stats;
  dst->drop_invalidated_pages = dst->drop_invalidated_pages;
  return 0;
}

int display_rt_params(buf_rt_params_t *rt_params, char *str)
{
  return sprintf(str, "rt_params: dirty_page_hotness=%d min_free_pages_per_bank=%d log_fault_sequence=%d fs_tag=%d drop_invalidated_pages=%d",
                 rt_params->dirty_page_hotness, rt_params->min_free_pages_per_bank, rt_params->log_fault_sequence, rt_params->fault_sequence_tag, rt_params->drop_invalidated_pages);
}

int print_rt_params(buf_rt_params_t *rt_params)
{
  return 0;
}

/*************************************************************************
 * Sysfs function interface
 ************************************************************************/
/* Extract the runtime parameters from the top level dimmap_buffer_t data structure */
static ssize_t buf_rt_params_show(struct class *class, struct class_attribute *attr, char *buf, ssize_t (*format)(buf_rt_params_t *rt_params, char *))
{
  buf_rt_params_t *rt_params = to_dimmap_buf_rt_params(class);
  ssize_t ret = -EINVAL;
  if (rt_params == NULL)
  {
    return scnprintf(buf, PAGE_SIZE, "NA: Buffer not initalized\n");
  }
  ret = (*format)(rt_params, buf);
  return ret;
}

/* Generate a show function for a runtime parameter field */
#define SHOW_RT_PARAM(field, format_string)                                                   \
  static ssize_t rt_param_format_##field(buf_rt_params_t *rt_params, char *buf)               \
  {                                                                                           \
    return scnprintf(buf, PAGE_SIZE, format_string, rt_params->field);                        \
  }                                                                                           \
  ssize_t show_rt_param_##field(struct class *class, struct class_attribute *attr, char *buf) \
  {                                                                                           \
    return buf_rt_params_show(class, attr, buf, rt_param_format_##field);                     \
  }

/* Generate a store function for an integer runtime parameter field */
#define STORE_INTEGER_RT_PARAM(field)                                                                              \
  ssize_t store_rt_param_##field(struct class *class, struct class_attribute *attr, const char *buf, size_t count) \
  {                                                                                                                \
    buf_rt_params_t *rt_params = to_dimmap_buf_rt_params(class);                                                   \
    int ret = -EINVAL;                                                                                             \
    int val = 0;                                                                                                   \
    ret = kstrtoint(buf, 0, &val);                                                                                 \
    if (ret)                                                                                                       \
    {                                                                                                              \
      goto err;                                                                                                    \
    }                                                                                                              \
    rt_params->field = val;                                                                                        \
    update_dimmap_buf_rt_params(class);                                                                            \
    ret = count;                                                                                                   \
  err:                                                                                                             \
    return ret;                                                                                                    \
  }

/* Generate a store function for an integer runtime parameter field */
#define TOGGLE_INTEGER_RT_PARAM(field, fn, test, reset)                                                             \
  ssize_t toggle_rt_param_##field(struct class *class, struct class_attribute *attr, const char *buf, size_t count) \
  {                                                                                                                 \
    buf_rt_params_t *rt_params = to_dimmap_buf_rt_params(class);                                                    \
    int ret = -EINVAL;                                                                                              \
    int val = 0;                                                                                                    \
    ret = kstrtoint(buf, 0, &val);                                                                                  \
    if (ret)                                                                                                        \
    {                                                                                                               \
      goto err;                                                                                                     \
    }                                                                                                               \
    rt_params->field = val;                                                                                         \
    if (rt_params->field == test)                                                                                   \
    {                                                                                                               \
      fn();                                                                                                         \
      rt_params->field = reset;                                                                                     \
    }                                                                                                               \
    update_dimmap_buf_rt_params(class);                                                                             \
    ret = count;                                                                                                    \
  err:                                                                                                              \
    return ret;                                                                                                     \
  }

SHOW_RT_PARAM(dirty_page_hotness, "%d\n")
STORE_INTEGER_RT_PARAM(dirty_page_hotness)

SHOW_RT_PARAM(min_free_pages_per_bank, "%d\n")
STORE_INTEGER_RT_PARAM(min_free_pages_per_bank)

SHOW_RT_PARAM(log_fault_sequence, "%d\n")
STORE_INTEGER_RT_PARAM(log_fault_sequence)

SHOW_RT_PARAM(fault_sequence_tag, "%d\n")
STORE_INTEGER_RT_PARAM(fault_sequence_tag)

SHOW_RT_PARAM(reset_history, "%d\n")
SHOW_RT_PARAM(print_status, "%d\n")
SHOW_RT_PARAM(reset_stats, "%d\n")
SHOW_RT_PARAM(drop_invalidated_pages, "%d\n")
STORE_INTEGER_RT_PARAM(drop_invalidated_pages)

void reset_history_fn(void)
{
  printk(KERN_ERR "DI-MMAP: Started reseting the histogram data for the buffer\n");
  reset_mmap_history();
  printk(KERN_ERR "DI-MMAP: Finished reseting the histogram data for the buffer\n");
}
TOGGLE_INTEGER_RT_PARAM(reset_history, reset_history_fn, 1, 0);

void print_status_fn(void)
{
  printk(KERN_ERR "ms_in_open = %ld\n", atomic64_read(&ms_in_open));
  printk(KERN_ERR "ms_in_fsync = %ld\n", atomic64_read(&ms_in_fsync));
  printk(KERN_ERR "ms_in_get_page = %ld\n", atomic64_read(&ms_in_get_page));

  print_status_dimmap_buffer();
}
TOGGLE_INTEGER_RT_PARAM(print_status, print_status_fn, 1, 0);

void reset_stats_fn(void)
{
  printk(KERN_ERR "DMAP: Reset stats and deallocate all files\n");

  atomic64_set(&ms_in_open, 0);
  atomic64_set(&ms_in_fsync, 0);
  atomic64_set(&ms_in_get_page, 0);

	ino_cache_clear();	

  reset_stats_dimmap_buffer();
}
TOGGLE_INTEGER_RT_PARAM(reset_stats, reset_stats_fn, 1, 0);

/* Buffer runtime parameters */
static struct class_attribute rt_param_class_attrs[] = {
    __ATTR(2_dirty_page_hotness, S_IRUGO | S_IWUSR, show_rt_param_dirty_page_hotness, store_rt_param_dirty_page_hotness),
    __ATTR(2_min_free_pages_per_bank, S_IRUGO | S_IWUSR, show_rt_param_min_free_pages_per_bank, store_rt_param_min_free_pages_per_bank),
    __ATTR(2_log_fault_sequence, S_IRUGO | S_IWUSR, show_rt_param_log_fault_sequence, store_rt_param_log_fault_sequence),
    __ATTR(2_fault_sequence_tag, S_IRUGO | S_IWUSR, show_rt_param_fault_sequence_tag, store_rt_param_fault_sequence_tag),
    __ATTR(2_reset_history, S_IRUGO | S_IWUSR, show_rt_param_reset_history, toggle_rt_param_reset_history),
    __ATTR(2_print_status, S_IRUGO | S_IWUSR, show_rt_param_print_status, toggle_rt_param_print_status),
    __ATTR(2_reset_stats, S_IRUGO | S_IWUSR, show_rt_param_reset_stats, toggle_rt_param_reset_stats),
    __ATTR(2_drop_invalidated_pages, S_IRUGO | S_IWUSR, show_rt_param_drop_invalidated_pages, store_rt_param_drop_invalidated_pages),
    __ATTR_NULL,
};

int copy_rt_param_class_attrs(struct class_attribute *global_class_attrs, int offset)
{
  int i = 0;
  while (rt_param_class_attrs[i].attr.name != NULL)
  {
    global_class_attrs[offset + i] = rt_param_class_attrs[i];
    i++;
  }
  return offset + i;
}
