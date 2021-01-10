#!/bin/bash

#echo 0 >/proc/sys/kernel/lock_stat
#echo 0 > /proc/lock_stat
#echo 1 >/proc/sys/kernel/lock_stat
#jemalloc.sh ./append-only-log read rand 8 /dev/dmap/dmap1
#echo 0 >/proc/sys/kernel/lock_stat
#cat /proc/lock_stat > out.txt
#grep : out.txt | head

#taskset -c 0 ./append-only-log 
#taskset -c 0-1 ./append-only-log 2
#taskset -c 0-3 ./append-only-log 4
#taskset -c 0-7 ./append-only-log 8
#taskset -c 0-15 ./append-only-log 16
#taskset -c 0-7,16-23 ./append-only-log 16
#taskset -c 0-23 ./append-only-log 24
#taskset -c 0-31 ./append-only-log 32

swapoff -a

for((i=0;i<32;i+=1))
do
	echo 'performance' > /sys/devices/system/cpu/cpu${i}/cpufreq/scaling_governor;
done

echo '**** read seq ****'
for i in `cat cores.txt`
do
	sync; echo 3 > /proc/sys/vm/drop_caches 
	jemalloc.sh ./append-only-log read seq $i /dev/dmap/dmap1
	sync; echo 3 > /proc/sys/vm/drop_caches 
done

echo '**** read rand ****'
for i in `cat cores.txt`
do
	sync; echo 3 > /proc/sys/vm/drop_caches 
	jemalloc.sh ./append-only-log read rand $i /dev/dmap/dmap1
	sync; echo 3 > /proc/sys/vm/drop_caches 
done

echo '**** write seq ****'
for i in `cat cores.txt`
do
	sync; echo 3 > /proc/sys/vm/drop_caches 
	jemalloc.sh ./append-only-log write seq $i /dev/dmap/dmap1
	sync; echo 3 > /proc/sys/vm/drop_caches 
done

echo '**** write rand ****'
for i in `cat cores.txt`
do
	sync; echo 3 > /proc/sys/vm/drop_caches 
	jemalloc.sh ./append-only-log write rand $i /dev/dmap/dmap1
	sync; echo 3 > /proc/sys/vm/drop_caches 
done
