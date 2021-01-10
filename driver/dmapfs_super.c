#include "wrapfs.h"

/*
 * The inode cache is used with alloc_inode for both our inode info and the
 * vfs inode.
 */
static struct kmem_cache *wrapfs_inode_cachep;

/* final actions when unmounting a file system */
static void wrapfs_put_super(struct super_block *sb)
{
	struct wrapfs_sb_info *spd;
	struct super_block *s;

	spd = WRAPFS_SB(sb);
	if (!spd)
		return;

	/* decrement lower super references */
	s = wrapfs_lower_super(sb);
	wrapfs_set_lower_super(sb, NULL);
	atomic_dec(&s->s_active);

	kfree(spd);
	sb->s_fs_info = NULL;
}

static int wrapfs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	int err;
	struct path lower_path;

	wrapfs_get_lower_path(dentry, &lower_path);
	err = vfs_statfs(&lower_path, buf);
	wrapfs_put_lower_path(dentry, &lower_path);

	/* set return buf to our f/s to avoid confusing user-level utils */
	buf->f_type = WRAPFS_SUPER_MAGIC;

	return err;
}

/*
 * @flags: numeric mount options
 * @options: mount options string
 */
static int wrapfs_remount_fs(struct super_block *sb, int *flags, char *options)
{
	int err = 0;

	/*
	 * The VFS will take care of "ro" and "rw" flags among others.  We
	 * can safely accept a few flags (RDONLY, MANDLOCK), and honor
	 * SILENT, but anything else left over is an error.
	 */
	if ((*flags & ~(MS_RDONLY | MS_MANDLOCK | MS_SILENT)) != 0) {
		printk(KERN_ERR
		       "wrapfs: remount flags 0x%x unsupported\n", *flags);
		err = -EINVAL;
	}

	return err;
}

/*
 * Called by iput() when the inode reference count reached zero
 * and the inode is not hashed anywhere.  Used to clear anything
 * that needs to be, before the inode is completely destroyed and put
 * on the inode free list.
 */
static void wrapfs_evict_inode(struct inode *inode)
{
	struct inode *lower_inode;

	truncate_inode_pages(&inode->i_data, 0);
	clear_inode(inode);
	/*
	 * Decrement a reference to a lower_inode, which was incremented
	 * by our read_inode when it was created initially.
	 */
	lower_inode = wrapfs_lower_inode(inode);
	wrapfs_set_lower_inode(inode, NULL);
	iput(lower_inode);
}

static struct inode *wrapfs_alloc_inode(struct super_block *sb)
{
	struct wrapfs_inode_info *i;

	i = kmem_cache_alloc(wrapfs_inode_cachep, GFP_KERNEL);
	if (!i)
		return NULL;

	/* memset everything up to the inode to 0 */
	memset(i, 0, offsetof(struct wrapfs_inode_info, vfs_inode));

	i->vfs_inode.i_version = 1;
	return &i->vfs_inode;
}

static void wrapfs_destroy_inode(struct inode *inode)
{
	kmem_cache_free(wrapfs_inode_cachep, WRAPFS_I(inode));
}

/* wrapfs inode cache constructor */
static void init_once(void *obj)
{
	struct wrapfs_inode_info *i = obj;

	inode_init_once(&i->vfs_inode);
}

int wrapfs_init_inode_cache(void)
{
	int err = 0;

	wrapfs_inode_cachep =
		kmem_cache_create("wrapfs_inode_cache",
				  sizeof(struct wrapfs_inode_info), 0,
				  SLAB_RECLAIM_ACCOUNT, init_once);
	if (!wrapfs_inode_cachep)
		err = -ENOMEM;
	return err;
}

/* wrapfs inode cache destructor */
void wrapfs_destroy_inode_cache(void)
{
	if (wrapfs_inode_cachep)
		kmem_cache_destroy(wrapfs_inode_cachep);
}

/*
 * Used only in nfs, to kill any pending RPC tasks, so that subsequent
 * code can actually succeed and won't leave tasks that need handling.
 */
static void wrapfs_umount_begin(struct super_block *sb)
{
	struct super_block *lower_sb;

	lower_sb = wrapfs_lower_super(sb);
	if (lower_sb && lower_sb->s_op && lower_sb->s_op->umount_begin)
		lower_sb->s_op->umount_begin(lower_sb);
}

const struct super_operations wrapfs_sops = {
	.put_super	= wrapfs_put_super,
	.statfs		= wrapfs_statfs,
	.remount_fs	= wrapfs_remount_fs,
	.evict_inode	= wrapfs_evict_inode,
	.umount_begin	= wrapfs_umount_begin,
	.alloc_inode	= wrapfs_alloc_inode,
	.destroy_inode	= wrapfs_destroy_inode,
	.drop_inode	= generic_delete_inode,
};

/* NFS support */

static struct inode *wrapfs_nfs_get_inode(struct super_block *sb, u64 ino,
					  u32 generation)
{
	struct super_block *lower_sb;
	struct inode *inode;
	struct inode *lower_inode;

	lower_sb = wrapfs_lower_super(sb);
	lower_inode = ilookup(lower_sb, ino);
	inode = wrapfs_iget(sb, lower_inode);
	return inode;
}

static struct dentry *wrapfs_fh_to_dentry(struct super_block *sb,
					  struct fid *fid, int fh_len,
					  int fh_type)
{
	return generic_fh_to_dentry(sb, fid, fh_len, fh_type,
				    wrapfs_nfs_get_inode);
}

static struct dentry *wrapfs_fh_to_parent(struct super_block *sb,
					  struct fid *fid, int fh_len,
					  int fh_type)
{
	return generic_fh_to_parent(sb, fid, fh_len, fh_type,
				    wrapfs_nfs_get_inode);
}

/*
 * all other funcs are default as defined in exportfs/expfs.c
 */

const struct export_operations wrapfs_export_ops = {
	.fh_to_dentry	   = wrapfs_fh_to_dentry,
	.fh_to_parent	   = wrapfs_fh_to_parent
};
