#!/bin/bash

# replace with the underlying device
DEVICE=/dev/XXX

set -x

echo 1 > /proc/sys/kernel/sysrq
echo 8 > /proc/sys/kernel/printk

modprobe dmap

echo 1 > /sys/class/dmap/0_buffer_state

../ioctl/set ${DEVICE}

# At this point a new device /dev/dmap/dmap1 will 
# appear. Using mmap() on this device will use
# the FastMap's optimized path.
