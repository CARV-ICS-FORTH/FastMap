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

size_t getFilesize(const char *filename)
{
	struct stat st;
	stat(filename, &st);
	return st.st_size;
}

int main(int argc, char *argv[])
{
	size_t size;
	void *map = NULL;
	int ret, fd;
	int gsum[2] = {0,0};

	if(argc != 2){
		printf("Usage: %s <file name>\n", argv[0]);
		exit(EXIT_FAILURE);
	}

	fd = open(argv[1], O_RDWR);
	if(fd == -1){
		perror("open");
		exit(EXIT_FAILURE);
	}

	size = getFilesize(argv[1]);
	fprintf(stderr, "file size is %zu bytes!\n", size);

	map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if(map == MAP_FAILED){
		perror("mmap");
		exit(EXIT_FAILURE);
	}
	fprintf(stderr, "mmap at [%llu][%p]\n", (unsigned long long)map, map);

	////////////////////////////
	
	int *mbuf = (int *)map;
	gsum[0] = mbuf[0];
	printf("gsum0 = %d\n", gsum[0]);

	mbuf = (int *)((char *)map + 4096);
	gsum[1] += mbuf[0];
	printf("gsum1 = %d\n", gsum[1]);

	////////////////////////////
	
	fprintf(stderr, "munmap at [%llu][%p] - [%zu]bytes\n", (unsigned long long)map, map, size);
	ret = munmap(map, size);
	if(ret != 0){
		perror("munmap");
		exit(EXIT_FAILURE);
	}

	close(fd);

	return 0;
}
