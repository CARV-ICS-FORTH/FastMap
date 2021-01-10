CFLAGS=-Wall -O0 -g

default: all

all: fsmall fsmall_write fsmall_read small test rand_threads_read rand_threads_write print_stats print_stats_csv prefetch dontneed set_file_priority

rand_threads_write: rand_threads_write.c
	gcc ${CFLAGS} rand_threads_write.c -o rand_threads_write -lpthread

rand_threads_read: rand_threads_read.c
	gcc ${CFLAGS} rand_threads_read.c -o rand_threads_read -lpthread

test: test.c
	gcc ${CFLAGS} -I../driver/ test.c -o test

small: small.c
	gcc ${CFLAGS} -I../driver/ small.c -o small

fsmall: fsmall.c
	gcc ${CFLAGS} -I../driver/ fsmall.c -o fsmall

fsmall_write: fsmall_write.c
	gcc ${CFLAGS} -I../driver/ fsmall_write.c -o fsmall_write

fsmall_read: fsmall_read.c
	gcc ${CFLAGS} -I../driver/ fsmall_read.c -o fsmall_read

print_stats: print_stats.c
	gcc ${CFLAGS} -I../driver/ print_stats.c -o print_stats

print_stats_csv: print_stats_csv.c
	gcc ${CFLAGS} -I../driver/ print_stats_csv.c -o print_stats_csv

prefetch: prefetch.c
	gcc ${CFLAGS} -I../driver/ prefetch.c -o prefetch

dontneed: dontneed.c
	gcc ${CFLAGS} -I../driver/ dontneed.c -o dontneed

set_file_priority: set_file_priority.c
	gcc ${CFLAGS} -I../driver/  set_file_priority.c -o set_file_priority
	
clean:
	rm -f fsmall_write fsmall_read fsmall small test rand_threads_read rand_threads_write print_stats print_stats_csv prefetch dontneed set_file_priority
