# FastMap

This library provides an optimized memory-mapped I/O path inside the Linux kernel. It is implemented as a kernel module and supported by user-space libraries for management purposes.

## Prerequisites

FastMap runs on a modified version of Linux 4.14. In order to build and run 
this version of FastMap you can clone this kernel with:
```bash
git clone https://github.com/tpapagian/linux-4.14.72-spf.git -b fastmap
```

The modifications are minimal as they contain only some exported symbols and 
the addition of 2 fields in a single struct. The specific modification can
be found at this [commit](https://github.com/tpapagian/linux-4.14.72-spf/commit/fdca6433c36bad7977ca019470225c54a4ef8fb7).

After that you should configure, build and install this kernel using 
specific instruction based on your setup.

## Building FastMap

In order to build FastMap simply run ```make``` on the top level directory. This will build the driver (located in driver directory) and 
the associated user space tools (located in ioctl directory). After that it will also install FastMap module and run ```depmod```.

## Running FastMap

The only required parameter is the number of 4KB DRAM pages that it will use as cache. You can define this in the ```driver/main.c``` file in the
```perma_mmap_buf_size``` variable. Alternatively you can use this as a command line argument (i.e. ```modprobe dmap perma_mmap_buf_size=1024``` and in this case it will allocate 1024 4KB pages tottalling in 4GB of DRAM cache).

In ```scripts``` directory you can find several scripts (with the appropriate comments) on how to laod and unload FastMap. These include:
| Script                      | Description                                                                                     |
|-----------------------------|----------------------------------------|
| scripts/load-it-blkdev.sh   | Load FastMap for a block device.       |
| scripts/unload-it-blkdev.sh | Unload FastMap for a block device.     |
| scripts/load-it-fs.sh       | Load FastMap for a file system.        |
| scripts/unload-it-fs.sh     | Unload FastMap for a file system.      |

## Design and Evaluation

The following paper presents the design and experimental analysis of FastMap.
```
Anastasios Papagiannis, Giorgos Xanthakis, Giorgos Saloustros, Manolis Marazakis, and Angelos Bilas. Optimizing Memory-mapped I/O for Fast Storage Devices. In proceedings of the 2020 USENIX Annual Technical Conference (USENIX ATC 20). July 2020.
```
The pdf, slides, and presentation can be found [here](https://www.usenix.org/conference/atc20/presentation/papagiannis).

Please cite this publication if you use/modify/evaluate FastMap.

## Funding 

This work is supported by [EVOLVE](https://www.evolve-h2020.eu/) (Grant Agreement ID: 825061) and [ExaNeSt](http://www.exanest.eu/) (Grant Agreement ID: 671553) EU funded projects.