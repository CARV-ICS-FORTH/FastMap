#ifndef __DIMMAP_CTRL_H
#define __DIMMAP_CTRL_H

#include <linux/types.h>

#define RAW_SETBIND _IO(0xac, 0)
#define RAW_GETBIND _IO(0xac, 1)

#define DIMMAP_REGISTER_WBQ_EVENTFD _IO(0xac, 2)
#define DIMMAP_CLEANUP_WBQ_EVENTFD _IO(0xac, 3)
#define DIMMAP_REGISTER_BKSTORE_EVENTFD _IO(0xac, 4)
#define DIMMAP_CLEANUP_BKSTORE_EVENTFD _IO(0xac, 5)
#define DIMMAP_REGISTER_CTRL_EVENTFD _IO(0xac, 6)
#define DIMMAP_CLEANUP_CTRL_EVENTFD _IO(0xac, 7)

#define MAX_FILENAME_LEN 500 //512

/* Make the data structure a total of 512 bytes */
typedef struct ui_file_struct
{
  unsigned short valid;
  unsigned short allows_directIO;
  unsigned int seq;
  unsigned int bk_dev_id;
  char filename[MAX_FILENAME_LEN];
} ui_file_t;

#define MAX_NUM_FILES 512 /* Currently support 512 unique files being open */

struct dimmap_config_request
{
  int raw_minor;
  __u64 block_major;
  __u64 block_minor;
  char filename[MAX_FILENAME_LEN];
};

#define MAX_DI_MMAP_MINORS 256 //CONFIG_MAX_RAW_DEVS
#define MAX_NUM_BANK_SETS 64

#endif /* __DIMMAP_CTRL_H */
