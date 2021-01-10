#include <linux/mm.h>

#include "dmap.h"
#include "buffer_loadtime_parameters.h"

int init_ld_params(buf_ld_params_t *ld_params)
{
  ld_params->mmap_buf_size = 0;
  ld_params->superpage_alloc_order = 0;
  ld_params->hot_page_fifo_alloc_ratio.mul = 1;
  ld_params->hot_page_fifo_alloc_ratio.logb2_shift = HPF_ALLOC_RATIO_SHIFT;
  ld_params->victim_queue_alloc_ratio.mul = 1;
  ld_params->victim_queue_alloc_ratio.logb2_shift = VQ_ALLOC_RATIO_SHIFT;
  return 0;
}

int copy_ld_params(buf_ld_params_t *dst, buf_ld_params_t *src)
{
  dst->mmap_buf_size = src->mmap_buf_size;
  dst->superpage_alloc_order = src->superpage_alloc_order;
  dst->hot_page_fifo_alloc_ratio.mul = src->hot_page_fifo_alloc_ratio.mul;
  dst->hot_page_fifo_alloc_ratio.logb2_shift = src->hot_page_fifo_alloc_ratio.logb2_shift;
  dst->victim_queue_alloc_ratio.mul = src->victim_queue_alloc_ratio.mul;
  dst->victim_queue_alloc_ratio.logb2_shift = src->victim_queue_alloc_ratio.logb2_shift;
  return 0;
}

int display_ld_params(buf_ld_params_t *ld_params, char *str)
{
  return sprintf(str, "ld_params: mmap_buf_size=%ld superpage_alloc_order=%d",
                 ld_params->mmap_buf_size, ld_params->superpage_alloc_order);
}

int print_ld_params(buf_ld_params_t *ld_params)
{
  return 0;
}

/*************************************************************************
 * Sysfs function interface
 ************************************************************************/
/* Extract the load-time parameters from the top level dimmap_buffer_t data structure */
static ssize_t buf_ld_params_show(struct class *class, struct class_attribute *attr, char *buf, ssize_t (*format)(buf_ld_params_t *ld_params, char *))
{
  buf_ld_params_t *ld_params = to_dimmap_buf_ld_params(class);
  ssize_t ret = -EINVAL;
  ret = (*format)(ld_params, buf);
  return ret;
}

/* Generate a show function for a load-time parameter field */
#define SHOW_LD_PARAM(field, format_string)                                                   \
  static ssize_t ld_param_format_##field(buf_ld_params_t *ld_params, char *buf)               \
  {                                                                                           \
    return scnprintf(buf, PAGE_SIZE, format_string, ld_params->field);                        \
  }                                                                                           \
  ssize_t show_ld_param_##field(struct class *class, struct class_attribute *attr, char *buf) \
  {                                                                                           \
    return buf_ld_params_show(class, attr, buf, ld_param_format_##field);                     \
  }

/* Generate a show function for a runtime parameter field */
#define SHOW_LD_PARAM_TUPLE(field, f1, f2, format_string)                                     \
  static ssize_t ld_param_format_##field(buf_ld_params_t *ld_params, char *buf)               \
  {                                                                                           \
    int f2_ = 1 << ld_params->field.f2;                                                       \
    return scnprintf(buf, PAGE_SIZE, format_string, ld_params->field.f1, f2_);                \
  }                                                                                           \
  ssize_t show_ld_param_##field(struct class *class, struct class_attribute *attr, char *buf) \
  {                                                                                           \
    return buf_ld_params_show(class, attr, buf, ld_param_format_##field);                     \
  }

/* Generate a store function for an integer runtime parameter field */
#define STORE_INTEGER_LD_PARAM(field)                                                                              \
  ssize_t store_ld_param_##field(struct class *class, struct class_attribute *attr, const char *buf, size_t count) \
  {                                                                                                                \
    buf_ld_params_t *ld_params = to_dimmap_buf_ld_params(class);                                                   \
    int ret = -EINVAL;                                                                                             \
    int val = 0;                                                                                                   \
    ret = kstrtoint(buf, 0, &val);                                                                                 \
    if (ret)                                                                                                       \
    {                                                                                                              \
      goto err;                                                                                                    \
    }                                                                                                              \
    ld_params->field = val;                                                                                        \
    update_dimmap_buf_ld_params(class);                                                                            \
    ret = count;                                                                                                   \
  err:                                                                                                             \
    return ret;                                                                                                    \
  }

SHOW_LD_PARAM(mmap_buf_size, "%ld\n")

ssize_t store_ld_param_mmap_buf_size(struct class *class, struct class_attribute *attr, const char *buf, size_t count)
{
  dimmap_buffer_t *dimmap_buf = to_dimmap_buf(class);
  buf_ld_params_t *ld_params = to_dimmap_buf_ld_params(class);
  int ret = -EINVAL;
  long new_size;

  ret = kstrtoul(buf, 0, &new_size);
  if (ret)
  {
    goto err;
  }

  ld_params->mmap_buf_size = new_size >> ld_params->superpage_alloc_order;
  ret = count;
  update_dimmap_buf_ld_params(class);

  init_mmap_params(&dimmap_buf->ld_params);

err:
  return ret;
}

SHOW_LD_PARAM(superpage_alloc_order, "%d\n")
STORE_INTEGER_LD_PARAM(superpage_alloc_order)

SHOW_LD_PARAM_TUPLE(hot_page_fifo_alloc_ratio, mul, logb2_shift, "%d/%d\n");
SHOW_LD_PARAM_TUPLE(victim_queue_alloc_ratio, mul, logb2_shift, "%d/%d\n");

ssize_t store_ld_param_hot_page_fifo_alloc_ratio(struct class *class, struct class_attribute *attr, const char *buf, size_t count)
{
  dimmap_buffer_t *dimmap_buf = to_dimmap_buf(class);
  buf_ld_params_t *ld_params = to_dimmap_buf_ld_params(class);
  int ret = -EINVAL;
  int mul = 0, logb2_shift = 0, denominator = 0;
  int i;

  ret = sscanf(buf, "%d/%d", &mul, &denominator);
  if (ret != 2)
  {
    goto err;
  }
  for (i = 0; (1 << i) < denominator; i++)
  {
    logb2_shift = i + 1;
  }

  ret = count;
  if (1 << logb2_shift != denominator)
  {
    printk(KERN_ALERT "The specified denominator (%d) cannot be encoded as the logb2 (%d) of the provided value.\n", denominator, logb2_shift);
    goto err;
  }

  ld_params->hot_page_fifo_alloc_ratio.mul = mul;
  ld_params->hot_page_fifo_alloc_ratio.logb2_shift = logb2_shift;
  update_dimmap_buf_ld_params(class);

  init_mmap_params(&dimmap_buf->ld_params);

err:
  return ret;
}

ssize_t store_ld_param_victim_queue_alloc_ratio(struct class *class, struct class_attribute *attr, const char *buf, size_t count)
{
  dimmap_buffer_t *dimmap_buf = to_dimmap_buf(class);
  buf_ld_params_t *ld_params = to_dimmap_buf_ld_params(class);
  int ret = -EINVAL;
  int mul = 0, logb2_shift = 0, denominator = 0;
  int i;

  ret = sscanf(buf, "%d/%d", &mul, &denominator);
  if (ret != 2)
  {
    goto err;
  }
  for (i = 0; (1 << i) < denominator; i++)
  {
    logb2_shift = i + 1;
  }

  ret = count;
  if (1 << logb2_shift != denominator)
  {
    printk(KERN_ALERT "The specified denominator (%d) cannot be encoded as the logb2 (%d) of the provided value.\n", denominator, logb2_shift);
    goto err;
  }

  ld_params->victim_queue_alloc_ratio.mul = mul;
  ld_params->victim_queue_alloc_ratio.logb2_shift = logb2_shift;
  update_dimmap_buf_ld_params(class);

  init_mmap_params(&dimmap_buf->ld_params);

err:
  return ret;
}

/* Buffer loadtime parameters */
static struct class_attribute ld_param_class_attrs[] = {
    __ATTR(1_buffer_size, S_IRUGO | S_IWUSR, show_ld_param_mmap_buf_size, store_ld_param_mmap_buf_size),
    __ATTR(1_superpage_alloc_order, S_IRUGO | S_IWUSR, show_ld_param_superpage_alloc_order, store_ld_param_superpage_alloc_order),
    __ATTR(1_hot_page_fifo_alloc_ratio, S_IRUGO | S_IWUSR, show_ld_param_hot_page_fifo_alloc_ratio, store_ld_param_hot_page_fifo_alloc_ratio),
    __ATTR(1_victim_queue_alloc_ratio, S_IRUGO | S_IWUSR, show_ld_param_victim_queue_alloc_ratio, store_ld_param_victim_queue_alloc_ratio),
    __ATTR_NULL,
};

int copy_ld_param_class_attrs(struct class_attribute *global_class_attrs, int offset)
{
  int i = 0;
  while (ld_param_class_attrs[i].attr.name != NULL)
  {
    global_class_attrs[offset + i] = ld_param_class_attrs[i];
    i++;
  }
  return offset + i;
}
