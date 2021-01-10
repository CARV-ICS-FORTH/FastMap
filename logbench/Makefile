CFLAGS=-Wall -O3 
 
default: all

all: append-only-log append-only-per-thread-log append-only-per-process-log append-only-log-rw

append-only-log: append-only-log.cpp
	g++ -Wall -O3 -std=c++0x append-only-log.cpp -o append-only-log -pthread

append-only-log-rw: append-only-log-rw.cpp
	g++ -Wall -O3 -std=c++0x append-only-log-rw.cpp -o append-only-log-rw -pthread

append-only-per-thread-log: append-only-per-thread-log.c
	gcc ${CFLAGS} append-only-per-thread-log.c -o append-only-per-thread-log -pthread

append-only-per-process-log: append-only-per-process-log.c
	gcc ${CFLAGS} append-only-per-process-log.c -o append-only-per-process-log -pthread

clean:
	rm -f append-only-log append-only-per-thread-log append-only-per-process-log append-only-log-rw
