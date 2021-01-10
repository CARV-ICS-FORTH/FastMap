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
	unsigned int dev_major;
	struct stat sb;
	unsigned int maj, min;
	int err;

	if(argc != 2){
		fprintf(stderr, "Syntax error: %s <device name>\n", argv[0]);
		exit(EXIT_FAILURE);
	}
		
	memset(&sb, 0, sizeof(struct stat));
	memset(&rq, 0, sizeof(struct dmap_config_request));
	
	rq.raw_minor = 1; // we only support 1 device!
	strcpy(rq.dev_path, argv[1]);

	fd = open(argv[1], O_RDONLY);
	if(fd < -0){
		perror("open");
		exit(EXIT_FAILURE);
	}

	err = fstat(fd, &sb);
	if(err < 0){
		perror("stat");
		exit(EXIT_FAILURE);
	}

	if(S_ISBLK(sb.st_mode)){ // we only support block devices
		rq.block_major = major(sb.st_rdev);
		rq.block_minor = minor(sb.st_rdev);
			
		printf("device: %s has major %u and minor %u\n", argv[1], rq.block_major, rq.block_minor);
	}else{
		fprintf(stderr, "\"%s\" is not a block device!\n", argv[1]);
		exit(EXIT_FAILURE);
	}

	close(fd);

	fd = open("/dev/dmap/dmapctl", O_RDWR);
	ioctl(fd, DMAP_SETBIND, &rq);
	close(fd);

	return 0;
}
