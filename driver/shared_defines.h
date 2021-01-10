#ifndef __SHARED_DEFINES_H__
#define __SHARED_DEFINES_H__

#define NUM_QUEUES 32

#define MAX_RMAPS_PER_PAGE 3

#define USE_DIRECT_IO
#define PAGES_PER_PGFAULT 1 /* 81 is the largest value that works */

#define EVICTOR_THREADS 8

#define ETLB_ENTRIES 16

#define MAX_OPEN_FDS 256

#define NUM_FREELISTS 32

#if 0
#define DMAP_BGON(x) BUG_ON(x)
#else
#define DMAP_BGON(x) ;;
#endif

//#define ENABLE_STATS

//#define USE_PROC_SYS_VM_COUNTERS

//#define USE_MAP_PAGES // enabled only for files

//#define USE_RADIX_TREE_FOR_DIRTY

#define USE_WAITQUEUE_FOR_COLLISIONS

#define USE_PERCPU_RADIXTREE

/*
 * This does not work with libvmmalloc and ligra.
 */
//#define USE_PAGE_FAULT_FASTPATH

//#define USE_MAP_PAGES_FASTPATH

/********* Experimental **********/

//#define USE_HUGEPAGES // Under development

#define D_INVD  39
#define D_BLKD  1
#define D_FILE  2

#endif /* __SHARED_DEFINES_H__ */
