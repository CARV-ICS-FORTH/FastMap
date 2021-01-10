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
	int fd;
	int i, ret;
	unsigned char prio;
	void *map;	

	if(argc != 3){
		printf("Usage: %s <file name> <priority>\n", argv[0]);
		exit(EXIT_FAILURE);
	}

	prio = atoi(argv[2]);
	fd = open(argv[1], O_RDWR);
	if(fd == -1){
		perror("open");
		exit(EXIT_FAILURE);
	}

	size = getFilesize(argv[1]);

	// mmap and munmap to create the pvd
  map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0); 
  if(map == MAP_FAILED){
    perror("mmap");
    exit(EXIT_FAILURE);
  }
  ret = munmap(map, size);
  if(ret != 0){ 
    perror("munmap");
    exit(EXIT_FAILURE);
  }
	
	for(i = 0; i < size / 4096; i++){
		struct dmap_page_prio dpp = { .pageno = i, .prio = prio };
		if(ioctl(fd, DMAP_CHANGE_PAGE_PRIORITY, &dpp) != 0){
			perror("ioctl");
			exit(EXIT_FAILURE);
		}
	}

	close(fd);

	return 0;
}
