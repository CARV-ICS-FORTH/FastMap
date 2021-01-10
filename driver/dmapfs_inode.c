#include <linux/delay.h>

#include <linux/module.h>
#include <linux/mm.h>   /* everything */
#include <linux/errno.h>  /* error codes */
#include <linux/delay.h>
#include <linux/time.h>
#include <linux/pagemap.h>
#include <linux/blkdev.h>
#include <linux/version.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/major.h>
#include <linux/backing-dev.h>
#include <linux/moduleparam.h>
#include <linux/capability.h>
#include <linux/uio.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/gfp.h>
#include <linux/compat.h>
#include <linux/vmalloc.h>
#include <linux/delay.h>
#include <linux/bio.h>
#include <linux/random.h>
#include <linux/buffer_head.h>

#include <asm/pgtable.h>
#include <asm/segment.h>
#include <asm/uaccess.h>
#include <asm/tlbflush.h>
#include <asm/tlbflush.h>

#include "wrapfs.h"
#include "dmap.h"

extern spinlock_t ino_pvd_lock;
extern struct radix_tree_root ino_pvd_cache;

extern struct lpfifo_operations_struct *lpfifo_ops;
extern struct dfifo_operations_struct *dfifo_ops;

static int wrapfs_create(struct inode *dir, struct dentry *dentry,
			 umode_t mode, bool want_excl)
{
	int err;
	struct dentry *lower_dentry;
	struct dentry *lower_parent_dentry = NULL;
	struct path lower_path;

	wrapfs_get_lower_path(dentry, &lower_path);
	lower_dentry = lower_path.dentry;
	lower_parent_dentry = lock_parent(lower_dentry);

	err = vfs_create(d_inode(lower_parent_dentry), lower_dentry, mode,
			 want_excl);
	if (err)
		goto out;
	err = wrapfs_interpose(dentry, dir->i_sb, &lower_path);
	if (err)
		goto out;
	fsstack_copy_attr_times(dir, wrapfs_lower_inode(dir));
	fsstack_copy_inode_size(dir, d_inode(lower_parent_dentry));

out:
	unlock_dir(lower_parent_dentry);
	wrapfs_put_lower_path(dentry, &lower_path);
	return err;
}

static int wrapfs_link(struct dentry *old_dentry, struct inode *dir,
		       struct dentry *new_dentry)
{
	struct dentry *lower_old_dentry;
	struct dentry *lower_new_dentry;
	struct dentry *lower_dir_dentry;
	u64 file_size_save;
	int err;
	struct path lower_old_path, lower_new_path;

	file_size_save = i_size_read(d_inode(old_dentry));
	wrapfs_get_lower_path(old_dentry, &lower_old_path);
	wrapfs_get_lower_path(new_dentry, &lower_new_path);
	lower_old_dentry = lower_old_path.dentry;
	lower_new_dentry = lower_new_path.dentry;
	lower_dir_dentry = lock_parent(lower_new_dentry);

	err = vfs_link(lower_old_dentry, d_inode(lower_dir_dentry),
		       lower_new_dentry, NULL);
	if (err || !d_inode(lower_new_dentry))
		goto out;

	err = wrapfs_interpose(new_dentry, dir->i_sb, &lower_new_path);
	if (err)
		goto out;
	fsstack_copy_attr_times(dir, d_inode(lower_new_dentry));
	fsstack_copy_inode_size(dir, d_inode(lower_new_dentry));
	set_nlink(d_inode(old_dentry),
		  wrapfs_lower_inode(d_inode(old_dentry))->i_nlink);
	i_size_write(d_inode(new_dentry), file_size_save);
out:
	unlock_dir(lower_dir_dentry);
	wrapfs_put_lower_path(old_dentry, &lower_old_path);
	wrapfs_put_lower_path(new_dentry, &lower_new_path);
	return err;
}

static int wrapfs_unlink(struct inode *dir, struct dentry *dentry)
{
	int err;
#ifdef USE_PERCPU_RADIXTREE
	int cpu;
#endif
	struct dentry *lower_dentry;
	struct inode *lower_dir_inode = wrapfs_lower_inode(dir);
	struct dentry *lower_dir_dentry;
	struct path lower_path;

	wrapfs_get_lower_path(dentry, &lower_path);
	lower_dentry = lower_path.dentry;
	dget(lower_dentry);
	lower_dir_dentry = lock_parent(lower_dentry);
	
	err = vfs_unlink(lower_dir_inode, lower_dentry, NULL);

	/*
	 * Note: unlinking on top of NFS can cause silly-renamed files.
	 * Trying to delete such files results in EBUSY from NFS
	 * below.  Silly-renamed files will get deleted by NFS later on, so
	 * we just need to detect them here and treat such EBUSY errors as
	 * if the upper file was successfully deleted.
	 */
	if (err == -EBUSY && lower_dentry->d_flags & DCACHE_NFSFS_RENAMED)
		err = 0;
	if (err)
		goto out;
	fsstack_copy_attr_times(dir, lower_dir_inode);
	fsstack_copy_inode_size(dir, lower_dir_inode);
	set_nlink(d_inode(dentry), wrapfs_lower_inode(d_inode(dentry))->i_nlink);
	d_inode(dentry)->i_ctime = dir->i_ctime;

  if(S_ISREG(lower_dentry->d_inode->i_mode) && wrapfs_lower_inode(d_inode(dentry))->i_nlink == 0)
  {
    unsigned long ino = lower_dentry->d_inode->i_ino;
    struct pr_vma_data *pvd = NULL;

		bool removed = false;
		struct tagged_page *p;
		void **slot;
		struct radix_tree_iter iter;
		pgoff_t start = 0;

		spin_lock(&ino_pvd_lock);
		pvd = radix_tree_delete(&ino_pvd_cache, ino);
		spin_unlock(&ino_pvd_lock);

		//printk(KERN_ERR" Deallocating pvd for ino %lu with fp %p\n", pvd->ino, pvd->bk.filp);
	
    if(pvd != NULL){
      DMAP_BGON( (pvd->magic1 != PVD_MAGIC_1) || (pvd->magic2 != PVD_MAGIC_2) );
			atomic64_set(&pvd->during_unlink, 1);

      /* we have to clear radix_tree */
      while(true){
        p = NULL;

#ifdef USE_PERCPU_RADIXTREE
        for(cpu = 0; cpu < num_online_cpus(); cpu++){
          rcu_read_lock();
          radix_tree_for_each_slot(slot, &pvd->rdx[cpu], &iter, start){
            p = radix_tree_deref_slot(slot);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,14,0) && LINUX_VERSION_CODE < KERNEL_VERSION(4,15,0)
            radix_tree_iter_delete(&pvd->rdx[cpu], &iter, slot);
#endif
            break;
          }
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,9,0) && LINUX_VERSION_CODE < KERNEL_VERSION(4,10,0)
					if(p != NULL){
						radix_tree_delete(&pvd->rdx[cpu], p->page->index);
					}
#endif
          rcu_read_unlock();

          if(p != NULL)
            break;
        }
#else /* USE_PERCPU_RADIXTREE */
        rcu_read_lock();
        radix_tree_for_each_slot(slot, &pvd->rdx, &iter, start){
          p = radix_tree_deref_slot(slot);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,14,0) && LINUX_VERSION_CODE < KERNEL_VERSION(4,15,0)
          radix_tree_iter_delete(&pvd->rdx, &iter, slot);
#endif
          break;
        }
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,9,0) && LINUX_VERSION_CODE < KERNEL_VERSION(4,10,0)
				if(p != NULL){
					radix_tree_delete(&pvd->rdx, p->page->index);
				}
#endif /* USE_PERCPU_RADIXTREE */
        rcu_read_unlock();
#endif /* USE_PERCPU_RADIXTREE */

        if(p == NULL)
          break;

				/* 
				 * FIXME Should we take the lock of page *p* here?
				 * I think no. But...
				 */
				removed = false;

				while(trylock_tp(p, ULONG_MAX) != 0)
					wait_on_page_bit(p->page, PG_locked);
				DMAP_BGON(atomic_read(&p->in_use) == 0);

				if(atomic_read(&p->buffer_id) == BI_CLEAN){
					spin_lock(&buf_data->banks->primary_fifo_data[p->page->index % NUM_QUEUES]->qlock);
					lpfifo_ops->remove(buf_data->banks->primary_fifo_data[p->page->index % NUM_QUEUES], p);
					spin_unlock(&buf_data->banks->primary_fifo_data[p->page->index % NUM_QUEUES]->qlock);
					removed = true;
				}

				if(removed)
					free_page_lock(&(buf_data->banks->page_map), p); /* it requires the page locked */
				else
					unlock_tp(p);
      }
      
			pvd->is_valid = false;

			//if(pvd->meta.f.m != NULL)
			//	vfree(pvd->meta.f.m);

			kfree(pvd);
    }else{
			/* this inode has not a pvd */
			/* can this happen? */
			//printk(KERN_ERR"We don't have a pvd in unlink!\n");
		}
  }
	
	d_drop(dentry); /* this is needed, else LTP fails (VFS won't do it) */

out:
	unlock_dir(lower_dir_dentry);
	dput(lower_dentry);
	wrapfs_put_lower_path(dentry, &lower_path);
	return err;
}

static int wrapfs_symlink(struct inode *dir, struct dentry *dentry,
			  const char *symname)
{
	int err;
	struct dentry *lower_dentry;
	struct dentry *lower_parent_dentry = NULL;
	struct path lower_path;

	wrapfs_get_lower_path(dentry, &lower_path);
	lower_dentry = lower_path.dentry;
	lower_parent_dentry = lock_parent(lower_dentry);

	err = vfs_symlink(d_inode(lower_parent_dentry), lower_dentry, symname);
	if (err)
		goto out;
	err = wrapfs_interpose(dentry, dir->i_sb, &lower_path);
	if (err)
		goto out;
	fsstack_copy_attr_times(dir, wrapfs_lower_inode(dir));
	fsstack_copy_inode_size(dir, d_inode(lower_parent_dentry));

out:
	unlock_dir(lower_parent_dentry);
	wrapfs_put_lower_path(dentry, &lower_path);
	return err;
}

static int wrapfs_mkdir(struct inode *dir, struct dentry *dentry, umode_t mode)
{
	int err;
	struct dentry *lower_dentry;
	struct dentry *lower_parent_dentry = NULL;
	struct path lower_path;

	wrapfs_get_lower_path(dentry, &lower_path);
	lower_dentry = lower_path.dentry;
	lower_parent_dentry = lock_parent(lower_dentry);

	err = vfs_mkdir(d_inode(lower_parent_dentry), lower_dentry, mode);
	if (err)
		goto out;

	err = wrapfs_interpose(dentry, dir->i_sb, &lower_path);
	if (err)
		goto out;

	fsstack_copy_attr_times(dir, wrapfs_lower_inode(dir));
	fsstack_copy_inode_size(dir, d_inode(lower_parent_dentry));
	/* update number of links on parent directory */
	set_nlink(dir, wrapfs_lower_inode(dir)->i_nlink);

out:
	unlock_dir(lower_parent_dentry);
	wrapfs_put_lower_path(dentry, &lower_path);
	return err;
}

static int wrapfs_rmdir(struct inode *dir, struct dentry *dentry)
{
	struct dentry *lower_dentry;
	struct dentry *lower_dir_dentry;
	int err;
	struct path lower_path;

	wrapfs_get_lower_path(dentry, &lower_path);
	lower_dentry = lower_path.dentry;
	lower_dir_dentry = lock_parent(lower_dentry);

	err = vfs_rmdir(d_inode(lower_dir_dentry), lower_dentry);
	if (err)
		goto out;

	d_drop(dentry);	/* drop our dentry on success (why not VFS's job?) */
	if (d_inode(dentry))
		clear_nlink(d_inode(dentry));
	fsstack_copy_attr_times(dir, d_inode(lower_dir_dentry));
	fsstack_copy_inode_size(dir, d_inode(lower_dir_dentry));
	set_nlink(dir, d_inode(lower_dir_dentry)->i_nlink);

out:
	unlock_dir(lower_dir_dentry);
	wrapfs_put_lower_path(dentry, &lower_path);
	return err;
}

static int wrapfs_mknod(struct inode *dir, struct dentry *dentry, umode_t mode,
			dev_t dev)
{
	int err;
	struct dentry *lower_dentry;
	struct dentry *lower_parent_dentry = NULL;
	struct path lower_path;

	wrapfs_get_lower_path(dentry, &lower_path);
	lower_dentry = lower_path.dentry;
	lower_parent_dentry = lock_parent(lower_dentry);

	err = vfs_mknod(d_inode(lower_parent_dentry), lower_dentry, mode, dev);
	if (err)
		goto out;

	err = wrapfs_interpose(dentry, dir->i_sb, &lower_path);
	if (err)
		goto out;
	fsstack_copy_attr_times(dir, wrapfs_lower_inode(dir));
	fsstack_copy_inode_size(dir, d_inode(lower_parent_dentry));

out:
	unlock_dir(lower_parent_dentry);
	wrapfs_put_lower_path(dentry, &lower_path);
	return err;
}

/*
 * The locking rules in wrapfs_rename are complex.  We could use a simpler
 * superblock-level name-space lock for renames and copy-ups.
 */
static int wrapfs_rename(struct inode *old_dir, struct dentry *old_dentry,
			 struct inode *new_dir, struct dentry *new_dentry,
			 unsigned int flags)
{
	int err = 0;
	struct dentry *lower_old_dentry = NULL;
	struct dentry *lower_new_dentry = NULL;
	struct dentry *lower_old_dir_dentry = NULL;
	struct dentry *lower_new_dir_dentry = NULL;
	struct dentry *trap = NULL;
	struct path lower_old_path, lower_new_path;

	if (flags)
		return -EINVAL;

	wrapfs_get_lower_path(old_dentry, &lower_old_path);
	wrapfs_get_lower_path(new_dentry, &lower_new_path);
	lower_old_dentry = lower_old_path.dentry;
	lower_new_dentry = lower_new_path.dentry;
	lower_old_dir_dentry = dget_parent(lower_old_dentry);
	lower_new_dir_dentry = dget_parent(lower_new_dentry);

	trap = lock_rename(lower_old_dir_dentry, lower_new_dir_dentry);
	/* source should not be ancestor of target */
	if (trap == lower_old_dentry) {
		err = -EINVAL;
		goto out;
	}
	/* target should not be ancestor of source */
	if (trap == lower_new_dentry) {
		err = -ENOTEMPTY;
		goto out;
	}

	err = vfs_rename(d_inode(lower_old_dir_dentry), lower_old_dentry,
			 d_inode(lower_new_dir_dentry), lower_new_dentry,
			 NULL, 0);
	if (err)
		goto out;

	fsstack_copy_attr_all(new_dir, d_inode(lower_new_dir_dentry));
	fsstack_copy_inode_size(new_dir, d_inode(lower_new_dir_dentry));
	if (new_dir != old_dir) {
		fsstack_copy_attr_all(old_dir,
				      d_inode(lower_old_dir_dentry));
		fsstack_copy_inode_size(old_dir,
					d_inode(lower_old_dir_dentry));
	}

out:
	unlock_rename(lower_old_dir_dentry, lower_new_dir_dentry);
	dput(lower_old_dir_dentry);
	dput(lower_new_dir_dentry);
	wrapfs_put_lower_path(old_dentry, &lower_old_path);
	wrapfs_put_lower_path(new_dentry, &lower_new_path);
	return err;
}

static int wrapfs_readlink(struct dentry *dentry, char __user *buf, int bufsiz)
{
	int err;
	struct dentry *lower_dentry;
	struct path lower_path;

	wrapfs_get_lower_path(dentry, &lower_path);
	lower_dentry = lower_path.dentry;
	if (!d_inode(lower_dentry)->i_op ||
	    !d_inode(lower_dentry)->i_op->readlink) {
		err = -EINVAL;
		goto out;
	}

	err = d_inode(lower_dentry)->i_op->readlink(lower_dentry,
						    buf, bufsiz);
	if (err < 0)
		goto out;
	fsstack_copy_attr_atime(d_inode(dentry), d_inode(lower_dentry));

out:
	wrapfs_put_lower_path(dentry, &lower_path);
	return err;
}

static const char *wrapfs_get_link(struct dentry *dentry, struct inode *inode,
				   struct delayed_call *done)
{
	char *buf;
	int len = PAGE_SIZE, err;
	mm_segment_t old_fs;

	if (!dentry)
		return ERR_PTR(-ECHILD);

	/* This is freed by the put_link method assuming a successful call. */
	buf = kmalloc(len, GFP_KERNEL);
	if (!buf) {
		buf = ERR_PTR(-ENOMEM);
		return buf;
	}

	/* read the symlink, and then we will follow it */
	old_fs = get_fs();
	set_fs(KERNEL_DS);
	err = wrapfs_readlink(dentry, buf, len);
	set_fs(old_fs);
	if (err < 0) {
		kfree(buf);
		buf = ERR_PTR(err);
	} else {
		buf[err] = '\0';
	}
	set_delayed_call(done, kfree_link, buf);
	return buf;
}

static int wrapfs_permission(struct inode *inode, int mask)
{
	struct inode *lower_inode;
	int err;

	lower_inode = wrapfs_lower_inode(inode);
	err = inode_permission(lower_inode, mask);
	return err;
}

static int wrapfs_setattr(struct dentry *dentry, struct iattr *ia)
{
	int err;
	struct dentry *lower_dentry;
	struct inode *inode;
	struct inode *lower_inode;
	struct path lower_path;
	struct iattr lower_ia;

	inode = d_inode(dentry);

	/*
	 * Check if user has permission to change inode.  We don't check if
	 * this user can change the lower inode: that should happen when
	 * calling notify_change on the lower inode.
	 */
	err = setattr_prepare(dentry, ia);
	if (err)
		goto out_err;

	wrapfs_get_lower_path(dentry, &lower_path);
	lower_dentry = lower_path.dentry;
	lower_inode = wrapfs_lower_inode(inode);

	/* prepare our own lower struct iattr (with the lower file) */
	memcpy(&lower_ia, ia, sizeof(lower_ia));
	if (ia->ia_valid & ATTR_FILE)
		lower_ia.ia_file = wrapfs_lower_file(ia->ia_file);

	/*
	 * If shrinking, first truncate upper level to cancel writing dirty
	 * pages beyond the new eof; and also if its' maxbytes is more
	 * limiting (fail with -EFBIG before making any change to the lower
	 * level).  There is no need to vmtruncate the upper level
	 * afterwards in the other cases: we fsstack_copy_inode_size from
	 * the lower level.
	 */
	if (ia->ia_valid & ATTR_SIZE) {
		err = inode_newsize_ok(inode, ia->ia_size);
		if (err)
			goto out;
		truncate_setsize(inode, ia->ia_size);
	}

	/*
	 * mode change is for clearing setuid/setgid bits. Allow lower fs
	 * to interpret this in its own way.
	 */
	if (lower_ia.ia_valid & (ATTR_KILL_SUID | ATTR_KILL_SGID))
		lower_ia.ia_valid &= ~ATTR_MODE;

	/* notify the (possibly copied-up) lower inode */
	/*
	 * Note: we use d_inode(lower_dentry), because lower_inode may be
	 * unlinked (no inode->i_sb and i_ino==0.  This happens if someone
	 * tries to open(), unlink(), then ftruncate() a file.
	 */
	inode_lock(d_inode(lower_dentry));
	err = notify_change(lower_dentry, &lower_ia, /* note: lower_ia */
			    NULL);
	inode_unlock(d_inode(lower_dentry));
	if (err)
		goto out;

	/* get attributes from the lower inode */
	fsstack_copy_attr_all(inode, lower_inode);
	/*
	 * Not running fsstack_copy_inode_size(inode, lower_inode), because
	 * VFS should update our inode size, and notify_change on
	 * lower_inode should update its size.
	 */

out:
	wrapfs_put_lower_path(dentry, &lower_path);
out_err:
	return err;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,9,0) && LINUX_VERSION_CODE < KERNEL_VERSION(4,10,0)
static int wrapfs_getattr(struct vfsmount *mnt, struct dentry *dentry, struct kstat *stat)
{
	int err;
	struct kstat lower_stat;
	struct path lower_path;

	wrapfs_get_lower_path(dentry, &lower_path);
	err = vfs_getattr(&lower_path, &lower_stat);
	if (err)
		goto out;
	fsstack_copy_attr_all(d_inode(dentry),
			      d_inode(lower_path.dentry));
	generic_fillattr(d_inode(dentry), stat);
	stat->blocks = lower_stat.blocks;
out:
	wrapfs_put_lower_path(dentry, &lower_path);
	return err;
}
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(4,14,0) && LINUX_VERSION_CODE < KERNEL_VERSION(4,15,0)
static int wrapfs_getattr(const struct path *path, struct kstat *stat, u32 request_mask, unsigned int query_flags)
{
	int err;
	struct kstat lower_stat;
	struct path lower_path;
	struct dentry *dentry = path->dentry;

	wrapfs_get_lower_path(dentry, &lower_path);
	err = vfs_getattr(&lower_path, &lower_stat, request_mask, query_flags);
	if (err)
		goto out;
	fsstack_copy_attr_all(d_inode(dentry),
			      d_inode(lower_path.dentry));
	generic_fillattr(d_inode(dentry), stat);
	stat->blocks = lower_stat.blocks;
out:
	wrapfs_put_lower_path(dentry, &lower_path);
	return err;
}
#endif

static int
wrapfs_setxattr(struct dentry *dentry, struct inode *inode, const char *name,
		const void *value, size_t size, int flags)
{
	int err; struct dentry *lower_dentry;
	struct path lower_path;

	wrapfs_get_lower_path(dentry, &lower_path);
	lower_dentry = lower_path.dentry;
	if (!(d_inode(lower_dentry)->i_opflags & IOP_XATTR)) {
		err = -EOPNOTSUPP;
		goto out;
	}
	err = vfs_setxattr(lower_dentry, name, value, size, flags);
	if (err)
		goto out;
	fsstack_copy_attr_all(d_inode(dentry),
			      d_inode(lower_path.dentry));
out:
	wrapfs_put_lower_path(dentry, &lower_path);
	return err;
}

static ssize_t
wrapfs_getxattr(struct dentry *dentry, struct inode *inode,
		const char *name, void *buffer, size_t size)
{
	int err;
	struct dentry *lower_dentry;
	struct inode *lower_inode;
	struct path lower_path;

	wrapfs_get_lower_path(dentry, &lower_path);
	lower_dentry = lower_path.dentry;
	lower_inode = wrapfs_lower_inode(inode);
	if (!(d_inode(lower_dentry)->i_opflags & IOP_XATTR)) {
		err = -EOPNOTSUPP;
		goto out;
	}
	err = vfs_getxattr(lower_dentry, name, buffer, size);
	if (err)
		goto out;
	fsstack_copy_attr_atime(d_inode(dentry),
				d_inode(lower_path.dentry));
out:
	wrapfs_put_lower_path(dentry, &lower_path);
	return err;
}

static ssize_t
wrapfs_listxattr(struct dentry *dentry, char *buffer, size_t buffer_size)
{
	int err;
	struct dentry *lower_dentry;
	struct path lower_path;

	wrapfs_get_lower_path(dentry, &lower_path);
	lower_dentry = lower_path.dentry;
	if (!(d_inode(lower_dentry)->i_opflags & IOP_XATTR)) {
		err = -EOPNOTSUPP;
		goto out;
	}
	err = vfs_listxattr(lower_dentry, buffer, buffer_size);
	if (err)
		goto out;
	fsstack_copy_attr_atime(d_inode(dentry),
				d_inode(lower_path.dentry));
out:
	wrapfs_put_lower_path(dentry, &lower_path);
	return err;
}

static int
wrapfs_removexattr(struct dentry *dentry, struct inode *inode, const char *name)
{
	int err;
	struct dentry *lower_dentry;
	struct inode *lower_inode;
	struct path lower_path;

	wrapfs_get_lower_path(dentry, &lower_path);
	lower_dentry = lower_path.dentry;
	lower_inode = wrapfs_lower_inode(inode);
	if (!(lower_inode->i_opflags & IOP_XATTR)) {
		err = -EOPNOTSUPP;
		goto out;
	}
	err = vfs_removexattr(lower_dentry, name);
	if (err)
		goto out;
	fsstack_copy_attr_all(d_inode(dentry), lower_inode);
out:
	wrapfs_put_lower_path(dentry, &lower_path);
	return err;
}

const struct inode_operations wrapfs_symlink_iops = {
	.readlink	= wrapfs_readlink,
	.permission	= wrapfs_permission,
	.setattr	= wrapfs_setattr,
	.getattr	= wrapfs_getattr,
	.get_link	= wrapfs_get_link,
	.listxattr	= wrapfs_listxattr,
};

const struct inode_operations wrapfs_dir_iops = {
	.create		= wrapfs_create,
	.lookup		= wrapfs_lookup,
	.link		= wrapfs_link,
	.unlink		= wrapfs_unlink,
	.symlink	= wrapfs_symlink,
	.mkdir		= wrapfs_mkdir,
	.rmdir		= wrapfs_rmdir,
	.mknod		= wrapfs_mknod,
	.rename		= wrapfs_rename,
	.permission	= wrapfs_permission,
	.setattr	= wrapfs_setattr,
	.getattr	= wrapfs_getattr,
	.listxattr	= wrapfs_listxattr,
};

const struct inode_operations wrapfs_main_iops = {
	.permission	= wrapfs_permission,
	.setattr	= wrapfs_setattr,
	.getattr	= wrapfs_getattr,
	.listxattr	= wrapfs_listxattr,
};

static int wrapfs_xattr_get(const struct xattr_handler *handler,
			    struct dentry *dentry, struct inode *inode,
			    const char *name, void *buffer, size_t size)
{
	return wrapfs_getxattr(dentry, inode, name, buffer, size);
}

static int wrapfs_xattr_set(const struct xattr_handler *handler,
			    struct dentry *dentry, struct inode *inode,
			    const char *name, const void *value, size_t size,
			    int flags)
{
	if (value)
		return wrapfs_setxattr(dentry, inode, name, value, size, flags);

	DMAP_BGON(flags != XATTR_REPLACE);
	return wrapfs_removexattr(dentry, inode, name);
}

const struct xattr_handler wrapfs_xattr_handler = {
	.prefix = "",		/* match anything */
	.get = wrapfs_xattr_get,
	.set = wrapfs_xattr_set,
};

const struct xattr_handler *wrapfs_xattr_handlers[] = {
	&wrapfs_xattr_handler,
	NULL
};
