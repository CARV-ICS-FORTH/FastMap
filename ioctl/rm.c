#define _BSD_SOURCE
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>

#include "def.h"

int main(int argc, char **argv)
{
	struct dmap_config_request rq;
	int fd;

	memset(&rq, 0, sizeof(struct dmap_config_request));

	rq.raw_minor = 1;
	rq.block_major = 0;
	rq.block_minor = 0;

	fd = open("/dev/dmap/dmapctl", O_RDWR);

	ioctl(fd, DMAP_SETBIND, &rq);

	close(fd);

	return 0;
}
