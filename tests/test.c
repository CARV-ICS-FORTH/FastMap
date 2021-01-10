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

#include "dmap-ioctl.h"

size_t getFilesize(const char *filename)
{
	struct stat st;
	stat(filename, &st);
	return st.st_size;
}

int main(int argc, char *argv[])
{
	uint64_t size;
	void *map = NULL;
	uint64_t i, page_num;
	int ret;
	int fd;
	int rw = -1;
	int sum = 0;
	//struct dmap_page_prio dp;

	srand(time(NULL));
	
	if(argc != 4){
		printf("Usage: %s <device name> <R|W> <number of pages>\n", argv[0]);
		exit(EXIT_FAILURE);
	}

	if(!strcmp(argv[2], "R")){
		rw = 0;
	}else if(!strcmp(argv[2], "W")){
		rw = 1;
	}else{
		printf("Usage: %s <device name> <R|W> <number of pages>\n", argv[0]);
		exit(EXIT_FAILURE);
	}
	
	page_num = atoi(argv[3]);

	fd = open(argv[1], O_RDWR);
	if(fd == -1){
		perror("open");
		exit(EXIT_FAILURE);
	}

//	if(ioctl(fd, BLKGETSIZE64, &size) == -1){
//		perror("ioctl");
//		exit(EXIT_FAILURE);
//	}
	size = getFilesize(argv[1]);

//	for(i = 0; i < page_num; i++){
//		dp.pageno = i;
//		dp.prio = i;
//		ioctl(fd, DMAP_SET_PAGE_PRIORITY, &dp);
//	}

	map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if(map == MAP_FAILED){
		perror("mmap");
		exit(EXIT_FAILURE);
	}

	for(i = 0; i < page_num; i++){
		if(rw == 0){ // read
			//sum += *(int *)((char *)map + ((rand() % page_num) * 4096));
			sum += *(int *)((char *)map + (i * 4096));
		}else{ // write
			//*(int *)((char *)map + ((rand() % page_num) * 4096)) = 44;
			*(int *)((char *)map + (i * 4096)) = 44;
		}
	}

	ret = munmap(map, size);
	if(ret != 0){
		perror("munmap");
		exit(EXIT_FAILURE);
	}
	
	close(fd);

	return 0;
}
