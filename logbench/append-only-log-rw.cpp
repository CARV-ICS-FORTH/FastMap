#include <cstdint>
#include <random>
#include <algorithm>
#include <exception>

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

int fd = -1;

volatile uint64_t log_size;
volatile uint64_t log_offset = 0;

int rw = -1, sr = -1;

const long int item_size = 4096; // in bytes
const long int item_count = 10000000;

uint64_t thread_num;
double ops[128];

pthread_mutex_t lock;
pthread_barrier_t bar;

static long int get_dev_size(const char *filename);

#if 0
static void stick_this_thread_to_core(int core_id)
{
	int ret;
	int num_cores = sysconf(_SC_NPROCESSORS_ONLN);
	if(core_id < 0){
		perror("sysconf");
		exit(EXIT_FAILURE);
	}

	cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(core_id % num_cores, &cpuset);

  pthread_t current_thread = pthread_self();    
  ret = pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset);

	if(ret != 0){
		perror("pthread_setaffinity_np");
		exit(EXIT_FAILURE);
	}
}
#endif

void *run_seq(void *arg)
{
	uint64_t N = 0; 
	uint64_t thread_id = (uint64_t)arg;
	struct timeval start, end;
	long elapsed_usec;
	double elapsed_sec;
	char *buffer = (char *)malloc(item_size * sizeof(char));

	//stick_this_thread_to_core(thread_id);

	pthread_barrier_wait(&bar);

	gettimeofday(&start, NULL);
	while(true)
	{
		uint64_t __tmp;
		__tmp = __sync_fetch_and_add(&log_offset, item_size);

		if(__tmp >= (item_count * item_size) )
			break;

		++N;

		if(rw == 1){
			//memcpy(buffer, (void *)(log_addr + __tmp), item_size); // read
			pread(fd, buffer, item_size, __tmp);
		}else{ // rw == 2
			//memcpy((void *)(log_addr + __tmp), buffer, item_size); // write
			pwrite(fd, buffer, item_size, __tmp);
		}

	}
	gettimeofday(&end, NULL);
	
	pthread_barrier_wait(&bar);

	free(buffer);

	elapsed_usec = ((end.tv_sec * 1000000 + end.tv_usec) - (start.tv_sec * 1000000 + start.tv_usec));
	elapsed_sec = elapsed_usec / 1000000.0;

	//fprintf(stderr, "S[%" PRIu64 "][%" PRIu64"][%ld usec][%lf sec][%lf ops/sec]\n", thread_id, N, elapsed_usec, elapsed_sec, N / elapsed_sec);
	ops[thread_id] = N / elapsed_sec;

	return NULL;
}

void *run_random(void *arg)
{
	uint64_t thread_id = (uint64_t)arg;
	uint64_t items_per_thread = (log_size / item_size) / thread_num;
	uint64_t N = 0;
	uint64_t beg = thread_id * items_per_thread;
	uint64_t end = beg + items_per_thread - 1;
	struct timeval start, stop;
	long elapsed_usec;
	double elapsed_sec;
	char *buffer = (char *)malloc(item_size * sizeof(char));
	
  thread_local static std::random_device rd{};
	thread_local static std::mt19937 engine{rd()};
	thread_local static std::uniform_int_distribution<uint64_t> uniform(beg, end);
	
	//stick_this_thread_to_core(thread_id);
	
	pthread_barrier_wait(&bar);

	gettimeofday(&start, NULL);
	
	for(uint64_t i = 0; i < item_count / thread_num; ++i){
		uint64_t __tmp = uniform(engine) * item_size;

		++N;

		if(rw == 1){
			//memcpy(buffer, (void *)(log_addr + __tmp), item_size); // read
			pread(fd, buffer, item_size, __tmp);
		}else{ // rw == 2
			//memcpy((void *)(log_addr + __tmp), buffer, item_size); // write
			pwrite(fd, buffer, item_size, __tmp);
		}
	}
	
	gettimeofday(&stop, NULL);
	
	pthread_barrier_wait(&bar);

	free(buffer);

	elapsed_usec = ((stop.tv_sec * 1000000 + stop.tv_usec) - (start.tv_sec * 1000000 + start.tv_usec));
	elapsed_sec = elapsed_usec / 1000000.0;

	//fprintf(stderr, "R[%" PRIu64 "][%" PRIu64"][%ld usec][%lf sec][%lf ops/sec]\n", thread_id, N, elapsed_usec, elapsed_sec, N / elapsed_sec);
	ops[thread_id] = N / elapsed_sec;

	return NULL;
}

void wrong_usage(char **argv)
{
	fprintf(stderr, "Wrong arguments!\nExample usage: %s <read/write> <seq/rand> <thread num> <device>\n", argv[0]);
	exit(EXIT_FAILURE);
}

int main(int argc, char **argv)
{
	int err;
	char *filename = NULL;
	long int filesize;
	uint64_t i;
	double total_ops;
	pthread_t tid[256];

	if(argc == 5){
		if(strcmp(argv[1], "read") == 0)
			rw = 1;
		else if(strcmp(argv[1], "write") == 0)
			rw = 2;
		else
			wrong_usage(argv);
		
		if(strcmp(argv[2], "seq") == 0)
			sr = 1;
		else if(strcmp(argv[2], "rand") == 0)
			sr = 2;
		else
			wrong_usage(argv);
		
		thread_num = strtoull(argv[3], NULL, 0);

		filename = argv[4];
	}else
		wrong_usage(argv);

	if(sr == -1 || rw == -1 || filename == NULL)
		wrong_usage(argv);

	filesize = get_dev_size(filename);
	log_size = filesize;

#if 0
	fprintf(stderr, "[%s][%s]\n", (rw == 1)?("READ"):("WRITE"), (sr == 1)?("SEQUENTIAL"):("RANDOM"));

	fprintf(stderr, "Device %s is %ld bytes %ld KB %ld MB %ld GB\n", 
						filename, filesize,
						filesize / 1024,
						filesize / 1024 / 1024,
						filesize / 1024 / 1024 / 1024
					);

	fprintf(stderr, "Dataset is %lu bytes %lu KB %lu MB %lu GB\n", 
						item_count * item_size,
						(item_count * item_size) / 1024LU,
						(item_count * item_size) / 1024LU / 1024LU,
						(item_count * item_size) / 1024LU / 1024LU / 1024LU
					);

	fprintf(stderr, "Running with %" PRIu64 " threads\n", thread_num);
#endif

	if(item_count * item_size > filesize){
		fprintf(stderr, "Dataset is larger than device!\n");
		exit(EXIT_FAILURE);
	}

	fd = open(filename, O_RDWR);
	if(fd == -1){
		perror("open");
		exit(EXIT_FAILURE);
	}
	
	err = posix_fadvise(fd, 0, filesize, POSIX_FADV_RANDOM);
	if(err != 0)
		fprintf(stderr, "Error in posix_fadvise()\n");
	
	pthread_barrier_init(&bar, NULL, thread_num);

	if(pthread_mutex_init(&lock, NULL) != 0){
		fprintf(stderr, "pthread_mutex_init failed\n");
		exit(EXIT_FAILURE);
	}

	system("date");

	for(i = 0; i < thread_num; i++){ 
		if(sr == 1)
			err = pthread_create(&(tid[i]), NULL, &run_seq, (void *)i);
		else // sr == 2
			err = pthread_create(&(tid[i]), NULL, &run_random, (void *)i);
		if(err != 0){
			fprintf(stderr, "pthread_create failed\n");
			exit(EXIT_FAILURE);
		}
	}

	for(i = 0; i < thread_num; i++)
		pthread_join(tid[i], NULL);
	
	system("date");

	pthread_mutex_destroy(&lock);
	pthread_barrier_destroy(&bar);

	close(fd);

	total_ops = 0;
	for(i = 0; i < thread_num; i++)
		total_ops += ops[i];
	fprintf(stderr, "[overall][%s][%s][%2" PRIu64 "][%13.3lf ops/sec]\n", (rw == 1)?("READ"):("WRITE"), (sr == 1)?("SEQUENTIAL"):("RANDOM"), thread_num, total_ops);

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
