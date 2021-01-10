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
	//int sum = 0;

	
	if(argc != 2){
		printf("Usage: %s <device name>\n", argv[0]);
		exit(EXIT_FAILURE);
	}
	
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

#if 0
	sum += *(int *)c; // READ
	*(int *)c = 1; // WRITE
	*(int *)c = 2; // WRITE

	*(int *)(c + 4096) = 443; // WRITE
	sum += *(int *)(c + 4096); // READ
	sum += *(int *)(c + 4096); // READ
	sum += *(int *)(c + 4096); // READ
	sum += *(int *)(c + 4096); // READ
	
	sum += *(int *)(c + (2*4096)); // READ
	*(int *)(c + (2*4096)) = 443; // WRITE
#endif

	//printf("sum = %d\n", sum);

	printf("Accessing 0-127 pages\n");
	for(i = 0; i < 128; i++)
		*(int *)(c + (i*4096)) = 443;
	
	getchar();

	printf("msync\n");
	msync(map, size, MS_SYNC);

	getchar();
	
	printf("Accessing other pages\n");
	for(i = 128; i < 512; i++)
		*(int *)(c + (i*4096)) = 443;
	
	getchar();
	
	printf("msync\n");
	msync(map, size, MS_SYNC);

	//for(i = 16; i < 32; i++){
	//	getchar();
	//	*(int *)(c + (i*4096)) = 443;
	//}
	
	//getchar();
	//msync(map, size, MS_SYNC);
	//getchar();

	////////////////////////////
	ret = munmap(map, size);
	if(ret != 0){
		perror("munmap");
		exit(EXIT_FAILURE);
	}
	
	close(fd);

	return 0;
}
