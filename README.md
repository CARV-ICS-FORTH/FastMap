# FastMap

This library provides an optimized memory-mapped I/O path inside the Linux kernel. It is implemented as a kernel module and supported by user-space libraries for management purposes.

## Prerequisites

FastMap currently runs on any 4.14 Linux kernel, with no kernel modifications being necessary. A 4.14 kernel from the Linux Kernel Archives can be found
[here](https://mirrors.edge.kernel.org/pub/linux/kernel/v4.x/linux-4.14.123.tar.gz).

After that, you should configure, build, and install the kernel using specific instructions based on your setup.

## Building FastMap

In order to build FastMap simply run ```make``` on the top-level directory. This will build the driver (located in the driver directory) and the associated userspace tools (located in the ioctl directory). After that, it will also install the FastMap module and run ```depmod```.

## Running FastMap

The only required parameter is the number of 4KB DRAM pages that it will use as a cache. You can define this in the ```driver/main.c``` file in the
```perma_mmap_buf_size``` variable. Alternatively, you can use this as a command-line argument (i.e. ```modprobe dmap perma_mmap_buf_size=1024``` and in this case it will allocate 1024 4KB pages totaling 4GB of DRAM cache).

In ```scripts``` directory you can find several scripts (with the appropriate comments) on how to load and unload FastMap. These include:
| Script                      | Description                            |
|-----------------------------|----------------------------------------|
| scripts/load-it-blkdev.sh   | Load FastMap for a block device.       |
| scripts/unload-it-blkdev.sh | Unload FastMap for a block device.     |
| scripts/load-it-fs.sh       | Load FastMap for a file system.        |
| scripts/unload-it-fs.sh     | Unload FastMap for a file system.      |

## Design and Evaluation

The following paper presents the design and experimental analysis of FastMap.

*Anastasios Papagiannis, Giorgos Xanthakis, Giorgos Saloustros, Manolis Marazakis, and Angelos Bilas. Optimizing Memory-mapped I/O for Fast Storage Devices. In proceedings of the 2020 USENIX Annual Technical Conference (USENIX ATC 20). July 2020.*

The pdf, slides, and presentation can be found [here](https://www.usenix.org/conference/atc20/presentation/papagiannis).

Please cite this publication if you use/modify/evaluate FastMap.
