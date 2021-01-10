#define _GNU_SOURCE
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
	int count = 0;
#if 0
	struct dmap_page_stats stats;
	struct dmap_page_stats *s;
#endif

	if(argc != 3){
		printf("Usage: %s <file name> <count>\n", argv[0]);
		exit(EXIT_FAILURE);
	}

	count = atoi(argv[2]);

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

	//size = getFilesize(argv[1]);
	ioctl(fd, BLKGETSIZE64, &size);

	map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if(map == MAP_FAILED){
		perror("mmap");
		exit(EXIT_FAILURE);
	}

	fprintf(stderr, "mmap = %lx\n", (unsigned long)map);

//	mbuf = (int *)((char *)map);
//	fprintf(stderr, "%d ", *mbuf);
	
//	mbuf = (int *)((char *)map + 4096);
//	fprintf(stderr, "%d ", *mbuf);

	////////////////////////////
	for(i = 0; i < count; ++i){
		//if(i % 10 == 0){
		//	sleep(1);
		//	fprintf(stderr, "%d\n", i);
		//}
		*(int *)((char *)map + (i * 4096)) = 123;
	}
	//msync(map, size, MS_SYNC);
	//sleep(10);
	//for(i = 0; i < count; ++i){
	//	if(i % 10 == 0){
//			sleep(1);
//			fprintf(stderr, "%d\n", i);
//		}
//		*(int *)((char *)map + (i * 4096)) = 123;
//		*(int *)((char *)map + (i * 4096) + 128) = 1999;
//	}
	////////////////////////////
	fprintf(stderr, "\n");
	
	//msync(map, size, MS_SYNC);
	//sleep(1);
	//msync(map, size, MS_SYNC);

	ret = munmap(map, size);
	if(ret != 0){
		perror("munmap");
		exit(EXIT_FAILURE);
	}

#if 0
	stats.pageno = 0;
	ret = ioctl(fd, DMAP_GETPAGE_STATS, &stats);
	if(ret != 0)
		fprintf(stderr, "ioctl(DMAP_GETPAGE_STATS) failed!\n");

	s = &stats;

	fprintf(stderr, "|pgno|prio|pgf|pgfma|pgfmi|evic|evcl|evdi|sync|reads|mkw|mkwf|hitb|hitbv|hitbi|recov|\n");
	fprintf(stderr, "|%4llu|%4u|%3llu|%5llu|%5llu|%4llu|%4llu|%4llu|%4llu|%5llu|%3llu|%4llu|%4llu|%5llu|%5llu|%5llu|\n",
									s->pageno, s->priority, s->num_page_faults, s->major_page_faults, s->minor_page_faults,
									s->num_evict, s->num_evict_clean, s->num_evict_dirty, s->num_syncs, s->num_reads,
									s->num_mkwrite, s->num_mkwrite_failed, s->num_hit_in_buffer, s->num_hit_in_buffer_valid,
									s->num_hit_in_buffer_invalid, s->num_recovered);

	stats.pageno = 13;
	ret = ioctl(fd, DMAP_GETPAGE_STATS, &stats);
	if(ret != 0)
		fprintf(stderr, "ioctl(DMAP_GETPAGE_STATS) failed!\n");
	
	s = &stats;
	fprintf(stderr, "|%4llu|%4u|%3llu|%5llu|%5llu|%4llu|%4llu|%4llu|%4llu|%5llu|%3llu|%4llu|%4llu|%5llu|%5llu|%5llu|\n",
									s->pageno, s->priority, s->num_page_faults, s->major_page_faults, s->minor_page_faults,
									s->num_evict, s->num_evict_clean, s->num_evict_dirty, s->num_syncs, s->num_reads,
									s->num_mkwrite, s->num_mkwrite_failed, s->num_hit_in_buffer, s->num_hit_in_buffer_valid,
									s->num_hit_in_buffer_invalid, s->num_recovered);
#endif
	close(fd);

	return 0;
}
