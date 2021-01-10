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

#define PAGE_SIZE 4096

int main(int argc, char *argv[])
{
	uint64_t size;
	void *map = NULL;
	uint64_t i, j; //, page_num;
	int ret;
	int fd;
	//int sum = 0;
	struct fake_blk_page_num bpn;
	struct dmap_prefetch pr;
	
	if(argc != 2){
		printf("Usage: %s <device name>\n", argv[0]);
		exit(EXIT_FAILURE);
	}
	
	fd = open(argv[1], O_RDWR);
	if(fd == -1){
		perror("open");
		exit(EXIT_FAILURE);
	}

	if(ioctl(fd, BLKGETSIZE64, &size) != 0){
		perror("ioctl");
		exit(EXIT_FAILURE);
	}

	map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if(map == MAP_FAILED){
		perror("mmap");
		exit(EXIT_FAILURE);
	}
	////////////////////////////
	uint64_t num_pages = size / PAGE_SIZE;
	char *c = (char *)map;
		
	for(i = 0; i < num_pages; i++){
		bpn.num = i;
		if(ioctl(fd, FAKE_BLK_IOC_FILL_PAGE, &bpn) != 0){
			perror("ioctl");
			exit(EXIT_FAILURE);
		}
	}

	printf("Fill done!\n");

	for(j = 0; j < num_pages; j += 511){
		pr.wait = 1;
		pr.num_pages = 511;
		for(i = j; i < j + 511; i++)
			pr.pg_offset[i-j] = i;

		dmap_flush_victims(fd);

		if(dmap_prefetch_pages(fd, &pr) != 0){
			perror("ioctl");
			exit(EXIT_FAILURE);
		}

		for(i = j; i < j + 511; i++)
			*(int *)(c + (i * 4096)) = 998;

	}

	//printf("Reading pages [0-3]\n");
	//for(i = 0; i < 4; i++)
	//	sum += *(int *)(c + (i * 4096));

	//printf("sum = %d\n", sum);
	
	////////////////////////////
	ret = munmap(map, size);
	if(ret != 0){
		perror("munmap");
		exit(EXIT_FAILURE);
	}
	
	close(fd);

	return 0;
}
