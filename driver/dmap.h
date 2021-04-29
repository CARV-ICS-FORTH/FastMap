#ifndef __LINUX_DMAP_H
#define __LINUX_DMAP_H

#include <linux/types.h>
#include <linux/raw.h>
#include <linux/radix-tree.h>
#include <linux/mutex.h>
#include <linux/kref.h>

#include "mmap_fault_handler.h"
#include "di-mmap_parameters.h"
#include "shared_defines.h"

#define DMAP_SETBIND _IO(0xde, 0)
#define DMAP_GETBIND _IO(0xde, 1)

#define to_dimmap_buf(c) container_of(c, dimmap_buffer_t, dimmap_class)
#define BYTES_TO_BITS(nb) ((BITS_PER_LONG * (nb)) / sizeof(long))
#define MAX_DI_MMAP_MINORS 256

#define BIO_SPAGE_SIZE (sizeof(struct bio) + (PAGES_PER_PGFAULT * sizeof(struct bio_vec)))

struct dmap_config_request
{
	int raw_minor;
	__u64 block_major;
	__u64 block_minor;
	char dev_path[492];
};

typedef struct dimmap_buffer_struct
{
	mmap_buffer_t *buf_data;
	struct class dimmap_class;
	buf_ld_params_t ld_params;	 /* DI-MMAP loadtime parameters - top level copy to push into buffer during allocation */
	sys_rt_params_t sys_rt_params; /* Top-level DI-MMAP system runtime parameters */
} dimmap_buffer_t;

/*
 * This is one per page 
 */
struct device_meta
{
	/* Pages with priority 0 evicted latest */
	uint8_t priority; // 0 ~ 255

#ifdef ENABLE_STATS
	/* statistics */
	unsigned long num_page_faults;
	unsigned long major_page_faults;
	unsigned long minor_page_faults;

	unsigned long num_evict;
	unsigned long num_evict_clean;
	unsigned long num_evict_dirty;
	unsigned long num_syncs;
	unsigned long num_reads;

	unsigned long num_mkwrite;
	unsigned long num_mkwrite_failed;

	unsigned long num_hit_in_buffer;
	unsigned long num_hit_in_buffer_valid;
	unsigned long num_hit_in_buffer_invalid;
	unsigned long num_recovered;
#endif
};

/*to keep the parameters of HEutropia as the bitmap*/
struct fbd_bitmap
{
	uint64_t bitmap_size;
	unsigned long *bitmap;
};

#define PVD_MAGIC_1 (0xAD334879)
#define PVD_MAGIC_2 (0xDF367491)

struct evictor_tlb {
	union{
		struct vm_area_struct *vma;
		struct mm_struct *vm_mm;
	};
	unsigned long start;
	unsigned long end;
};

struct pr_vma_entry
{
	struct kref refcount;
	unsigned long vm_start, vm_end;	
	spinlock_t pcpu_mapped_lock[128]; /* FIXME max 128 cores */
	struct list_head pcpu_mapped[128]; /* FIXME max 128 cores */
};

struct pr_vma_data
{
	unsigned long magic1; // -----

	unsigned char type; /* can be D_INVD, D_FILE or D_BLKD */
	dev_t dev_id;		/* The device that this page -or- file is associated with */
	unsigned long ino;  /* file inode number - only valid if type == D_FILE */

	/* lower filesystem page_mkwrite - only valid if type == D_FILE */
	int (*lower_page_mkwrite)(struct vm_area_struct *, struct vm_fault *);

	atomic64_t cnt;
	atomic64_t mmaped;

	union {
		struct block_device *bdev;
		struct file *filp; /* lower file pointer */
	} bk;

	//
	struct file *open_fds[MAX_OPEN_FDS];
	//

	/* dirty tree */
	spinlock_t tree_lock[128];				/* FIXME max 128 cores */
#ifdef USE_RADIX_TREE_FOR_DIRTY
	struct radix_tree_root dirty_tree[128]; /* FIXME max 128 cores */
#else
	struct rb_root dirty_tree[128];	/* FIXME max 128 cores */
#endif

	/* metadata */
	union {
		struct raw_device_data *rdd; // block device metadata
#if 0
		struct
		{ // file metadata
			struct device_meta *m;
			loff_t file_size;
		} f;
#endif
	} meta;

	bool is_readonly;
	bool is_mmap;
	bool is_valid;

	/* used for hash_table of open files */
	struct hlist_node hchain;

	/* keeps mappings from (page_offset) to (struct tagged_page *) */
#ifdef USE_PERCPU_RADIXTREE
	spinlock_t radix_lock[128];			/* FIXME max 128 cores */
	struct radix_tree_root rdx[128];	/* FIXME max 128 cores */
#else
	spinlock_t radix_lock;	
	struct radix_tree_root rdx;
#endif

	/* used for vma_{open, close} */
	spinlock_t pvd_lock;

	atomic64_t vma_count;	
	atomic64_t during_unlink;

	unsigned long magic2; // -----
};

/*
 * Struct used to encapsulate
 * fastmap structs required in vm_area_struct,
 * stored in vm_private_data member
 */
struct fastmap_info {
	bool is_fastmap;
	struct pr_vma_data *pvd;
	struct pr_vma_entry *pve;
};

struct raw_device_data
{
	int inuse;

	dev_t dev_id; /* Device number: MAJOR,MINOR */
	struct block_device *binding;

	/* Track the number of concurrent accesses to each block ramdisk device */
	atomic_t num_concurrent_read_accesses;
	atomic_t num_concurrent_write_accesses;

	dimmap_buffer_t *buffer;

	struct device_meta *meta;
	unsigned long device_size_in_pages;
	struct fbd_bitmap bitmap;

	char path[128];
};

extern dimmap_buffer_t dimmap_buffer;

int allocate_and_init_runtime_buffer(dimmap_buffer_t *dimmap_rt);
int deallocate_runtime_buffer(dimmap_buffer_t *dimmap_rt);

#endif /* __LINUX_DMAP_H */
