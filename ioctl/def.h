#ifndef __LINUX_DMAP_H
#define __LINUX_DMAP_H

#include <linux/types.h>
#include <linux/raw.h>

#define DMAP_SETBIND  _IO( 0xde, 0 )
#define DMAP_GETBIND  _IO( 0xde, 1 )

struct dmap_config_request 
{
	int raw_minor;
	__u64 block_major;
	__u64 block_minor;
	char dev_path[492];
};

#define MAX_DMAP_MINORS CONFIG_MAX_RAW_DEVS

#endif
