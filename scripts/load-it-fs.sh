#!/bin/bash

# replace it with the underlying device of the backing filesystem
DEVICE=/dev/XXX

set -x

echo 1 > /proc/sys/kernel/sysrq
echo 8 > /proc/sys/kernel/printk

modprobe dmap

echo 1 > /sys/class/dmap/0_buffer_state

mkfs.xfs ${DEVICE} -f
mount ${DEVICE} /mnt/backing/
mount -t wrapfs /mnt/backing /mnt/fmap

# At this point any mmap call to any file in 
# /mnt/fmap will use FastMap's optimized path.
