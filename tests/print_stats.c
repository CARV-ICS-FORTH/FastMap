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

int main(int argc, char *argv[])
{
	int ret, fd;
	struct dmap_page_stats stats;
	struct dmap_page_stats *s;
	struct fake_blk_page_num pg;
	unsigned long i;

	if(argc != 2){
		printf("Usage: %s <file name>\n", argv[0]);
		exit(EXIT_FAILURE);
	}

	fd = open(argv[1], O_RDWR);
	if(fd == -1){
		perror("open");
		exit(EXIT_FAILURE);
	}

	ioctl(fd, FAKE_BLK_IOC_GET_DEVPGNUM, &pg);
	fprintf(stderr, "Decice has %llu pages.\n", pg.num);

	fprintf(stderr, "|pgno|prio|pgf|pgfma|pgfmi|evic|evcl|evdi|sync|reads|mkw|mkwf|hitb|hitbv|hitbi|recov|\n");
	for(i = 0; i < pg.num; i++){
		stats.pageno = i;
		ret = ioctl(fd, DMAP_GETPAGE_STATS, &stats);
		if(ret != 0){
			fprintf(stderr, "ioctl(DMAP_GETPAGE_STATS) for page %lu failed!\n", i);
			break;
		}

		s = &stats;
		if(s->priority != 0 || s->num_page_faults != 0){
			fprintf(stderr, "|%4llu|%4u|%3llu|%5llu|%5llu|%4llu|%4llu|%4llu|%4llu|%5llu|%3llu|%4llu|%4llu|%5llu|%5llu|%5llu|\n",
											s->pageno, s->priority, s->num_page_faults, s->major_page_faults, s->minor_page_faults,
											s->num_evict, s->num_evict_clean, s->num_evict_dirty, s->num_syncs, s->num_reads,
											s->num_mkwrite, s->num_mkwrite_failed, s->num_hit_in_buffer, s->num_hit_in_buffer_valid,
											s->num_hit_in_buffer_invalid, s->num_recovered);
		}
	}

	close(fd);

	return 0;
}
