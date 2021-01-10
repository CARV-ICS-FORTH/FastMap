#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <malloc.h>
#include <inttypes.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <unistd.h>
#include <time.h>

#include "../driver/dmap-ioctl.h"

int main(int argc, char *argv[])
{
	uint64_t size;
	void *map = NULL;
	uint64_t i; //, page_num;
	int ret;
	int fd;
	int r;
	int sum = 0;

	
	if(argc != 2){
		printf("Usage: %s <device name>\n", argv[0]);
		exit(EXIT_FAILURE);
	}
	
	srand(time(NULL));

	fd = open(argv[1], O_RDWR);
	if(fd == -1){
		perror("open");
		exit(EXIT_FAILURE);
	}

	if(ioctl(fd, BLKGETSIZE64, &size) == -1){
		perror("ioctl");
		exit(EXIT_FAILURE);
	}

	map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if(map == MAP_FAILED){
		perror("mmap");
		exit(EXIT_FAILURE);
	}
	////////////////////////////

	char *c = (char *)map;

	sum = 0;
	printf("Accessing 0-127 pages\n");
	for(i = 0; i < 128; i++){
		r = rand();
		*(int *)(c + (i*4096)) = r;
		sum += r;
	}
	fprintf(stderr, "sum=[%d]\n", sum);

	dmap_dontneed(fd, 0, 512);

	sum = 0;
	printf("Accessing 0-127 pages\n");
	for(i = 0; i < 128; i++){
		r = *(int *)(c + (i*4096));
		sum += r;
	}
	fprintf(stderr, "sum=[%d]\n", sum);

	////////////////////////////
	ret = munmap(map, size);
	if(ret != 0){
		perror("munmap");
		exit(EXIT_FAILURE);
	}
	
	close(fd);

	return 0;
}
