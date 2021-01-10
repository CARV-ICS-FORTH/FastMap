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
	int i;
	int *gbuf[2];
	int gsum[2] = {0,0};
	int msum[2] = {0,0};

	if(argc != 2){
		printf("Usage: %s <file name>\n", argv[0]);
		exit(EXIT_FAILURE);
	}

	srand(time(NULL));

	gbuf[0] = malloc(4096);
	for(i = 0; i < (4096/sizeof(int)); i++){
		gbuf[0][i] = rand();
		gsum[0] += gbuf[0][i];
	}
	printf("gsum0 = %d\n", gsum[0]);
	
	gbuf[1] = malloc(4096);
	for(i = 0; i < (4096/sizeof(int)); i++){
		gbuf[1][i] = rand();
		gsum[1] += gbuf[1][i];
	}
	printf("gsum1 = %d\n", gsum[1]);

	fd = open(argv[1], O_RDWR);
	if(fd == -1){
		perror("open");
		exit(EXIT_FAILURE);
	}
	if(pwrite(fd, gbuf[0], 4096, 0) != 4096){
		perror("write");
		exit(EXIT_FAILURE);
	}
	if(pwrite(fd, gbuf[1], 4096, 4096) != 4096){
		perror("write");
		exit(EXIT_FAILURE);
	}
	fsync(fd);
	close(fd);
	
	fd = open(argv[1], O_RDWR);
	if(fd == -1){
		perror("open");
		exit(EXIT_FAILURE);
	}

	size = getFilesize(argv[1]);

	map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if(map == MAP_FAILED){
		perror("mmap");
		exit(EXIT_FAILURE);
	}
	////////////////////////////
	int *mbuf = (int *)map;
	for(i = 0; i < (4096/sizeof(int)); i++){
		msum[0] += mbuf[i];
	}
	printf("msum0 = %d\n", msum[0]);

	mbuf = (int *)((char *)map + 4096);
	for(i = 0; i < (4096/sizeof(int)); i++){
		msum[1] += mbuf[i];
	}
	printf("msum1 = %d\n", msum[1]);
	////////////////////////////
	ret = munmap(map, size);
	if(ret != 0){
		perror("munmap");
		exit(EXIT_FAILURE);
	}

	close(fd);

	return 0;
}
