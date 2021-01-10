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
#include <pthread.h>

struct thread_data {
	char *map;
	uint64_t pages;
};

void *runner(void *data)
{
	struct thread_data *d = (struct thread_data *)data;
	uint64_t i;

	for(i = 0; i < (78643200); i++){ // 30GB
		*(int *)((char *)d->map + ((rand() % d->pages) * 4096)) = 996;
	//	if(i % 10000)
	//		fprintf(stderr, ".");
	}

	return (void *)0;
}

int main(int argc, char *argv[])
{
	uint64_t size;
	void *map = NULL;
	uint64_t i;
	int ret;
	int fd;
	int threads;
	pthread_t *thread_desc = NULL;
	struct thread_data thr_data;
	
	if(argc != 3){
		printf("please provide the device name and the number of threads!\n");
		exit(EXIT_FAILURE);
	}

	threads = atoi(argv[2]);

	printf("device is %s\n", argv[1]);
	printf("threads = %d\n", threads);
	
	srand(time(NULL));

	fd = open(argv[1], O_RDWR);
	if(fd == -1){
		printf("error 1\n");
		perror("open");
		exit(EXIT_FAILURE);
	}

	if(ioctl(fd, BLKGETSIZE64, &size) == -1){
		printf("error 2\n");
		perror("ioctl");
		exit(EXIT_FAILURE);
	}

	map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if(map == MAP_FAILED){
		printf("error 3\n");
		perror("mmap");
		exit(EXIT_FAILURE);
	}

	thr_data.map = map;
	thr_data.pages = size / 4096;

	thread_desc = malloc(threads * sizeof(pthread_t));	

	for(i = 0; i < threads; i++){
		if(pthread_create(&thread_desc[i], NULL, runner, &thr_data)){
			fprintf(stderr, "Error creating thread\n");
			return 1;
		}
	}

	for(i = 0; i < threads; i++){
		if(pthread_join(thread_desc[i], NULL)){
			fprintf(stderr, "Error joining thread\n");
			return 2;
		}
	}

	free(thread_desc);
	
	ret = munmap(map, size);
	if(ret != 0){
		printf("error 4\n");
		perror("munmap");
		exit(EXIT_FAILURE);
	}
	
	close(fd);

	return 0;
}
