#!/bin/bash

set -x

echo 1 > /sys/class/dmap/2_reset_stats

../ioctl/rmv

echo 0 > /sys/class/dmap/0_buffer_state

rmmod dmap

