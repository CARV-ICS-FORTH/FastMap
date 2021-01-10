#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/time.h>

#include <inttypes.h>
#include <fcntl.h>
#include <pthread.h>

#include "../driver/dmap-ioctl.h"

//#define DEVICE "/dev/fbd"
#define DEVICE "/dev/dmap/dmap1"

#define GB(x) (x * 1024 * 1024 * 1024UL)

#define MMAP_MEMORY GB(90)

uint64_t log_addr[1024];
uint64_t log_size;

const uint64_t item_size = 4096; // in bytes
const uint64_t item_count = 10000000;
uint64_t thread_num;
double ops[1024];

pthread_t tid[1024];
pthread_barrier_t barrier;
pthread_mutex_t lock;

static long int get_dev_size(const char *filename);

void *run(void *arg)
{
	uint64_t N = item_count / thread_num;
	uint64_t i;
	uint64_t thread_id = (uint64_t)arg;
	struct timeval start, end;
	long elapsed_usec;
	double elapsed_sec;

	uint64_t __log_addr = log_addr[thread_id];
	uint64_t __log_offset = 0;

	char *buffer = malloc(item_size * sizeof(char));

	pthread_barrier_wait(&barrier);
	gettimeofday(&start, NULL);
	for(i = 0; i < N; i++)
	{
		//memcpy((void *)(__log_addr + __log_offset), buffer, item_size);
		memcpy(buffer, (void *)(__log_addr + __log_offset), item_size);
		__log_offset += item_size;
	}
	gettimeofday(&end, NULL);
	
	free(buffer);

	elapsed_usec = ((end.tv_sec * 1000000 + end.tv_usec) - (start.tv_sec * 1000000 + start.tv_usec));
	elapsed_sec = elapsed_usec / 1000000.0;

	//fprintf(stderr, "[%" PRIu64 "][%" PRIu64"][%ld usec][%lf sec][%lf ops/sec]\n", thread_id, N, elapsed_usec, elapsed_sec, N / elapsed_sec);
	ops[thread_id] = N / elapsed_sec;

	return NULL;
}

int main(int argc, char **argv)
{
	int fd, err;
	char *filename = DEVICE;
	long int filesize;
	uint64_t i;
	double total_ops;
	struct fake_blk_page_range frang;

	if(argc == 2){
		thread_num = strtoull(argv[1], NULL, 0);
	}else{
		fprintf(stderr, "Wrong arguments!\nExample usage: %s <thread num>\n", argv[0]);
		exit(EXIT_FAILURE);
	}

	filesize = get_dev_size(filename);

	fprintf(stderr, "Device %s has size %ld bytes\n", filename, filesize);
	//fprintf(stderr, "Dataset is %lu MB bytes\n", (item_count * item_size) / 1024LU / 1024LU);
	//fprintf(stderr, "Running with %" PRIu64 " threads\n", thread_num);

	if(item_count * item_size > filesize){
		fprintf(stderr, "Dataset is larger than device!\n");
		exit(EXIT_FAILURE);
	}

	pthread_barrier_init(&barrier, NULL, thread_num);

	fd = open(filename, O_RDWR);
	if(fd == -1){
		perror("open");
		exit(EXIT_FAILURE);
	}
	
	log_size = filesize / thread_num;
	for(i = 0; i < thread_num; i++){
		log_addr[i] = (uint64_t)mmap(NULL, log_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, i * log_size);
		//log_addr[i] = (uint64_t)mmap(NULL, MMAP_MEMORY / thread_num, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
		if(log_addr[i] == (uint64_t)MAP_FAILED){
			perror("mmap");
			exit(EXIT_FAILURE);
		}
	}

	err = ioctl(fd, FAKE_BLK_IOC_TEST_CAP); // here we check if the device is a fake_blk device, maybe add another ioctl for this purpose
  if(err == 0){ // success call
    // we should also zero all range from start to size
    frang.offset = 0;  // convert from bytes to pages
    frang.length = log_size / 4096; // convert from bytes to pages

    err = ioctl(fd, FAKE_BLK_IOC_ZERO_RANGE, &frang);
    if(err){
      printf("ioctl(FAKE_BLK_IOC_ZERO_RANGE) failed! Program exiting...\n");
      exit(EXIT_FAILURE);
    }
  }

	for(i = 0; i < thread_num; i++){ 
		err = pthread_create(&(tid[i]), NULL, &run, (void *)i);
		if(err != 0){
			fprintf(stderr, "pthread_create failed\n");
			exit(EXIT_FAILURE);
		}
	}

	for(i = 0; i < thread_num; i++){
		munmap((void *)log_addr, log_size);
		//munmap((void *)log_addr, MMAP_MEMORY/thread_num);
		pthread_join(tid[i], NULL);
	}

	close(fd);

	total_ops = 0;
	for(i = 0; i < thread_num; i++)
		total_ops += ops[i];
	fprintf(stderr, "[overall][%2" PRIu64 "][%13.3lf ops/sec]\n", thread_num, total_ops);

	return 0;
}

static long int get_dev_size(const char *filename)
{
	long int size;
	int fd;
	
	fd = open(filename, O_RDONLY);
	if(fd == -1){
		perror("open");
		exit(EXIT_FAILURE);
	}

	if(ioctl(fd, BLKGETSIZE64, &size) == -1){
		fprintf(stderr, "[%s:%s:%d] querying file size\n",__FILE__,__func__,__LINE__);
		size = lseek(fd, 0, SEEK_END);
		if(size == -1){
			perror("lseek");
			exit(EXIT_FAILURE);
		}
	}

	close(fd);

	return size;
}
