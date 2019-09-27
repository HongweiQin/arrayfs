/*
 * Arrayfs is a simple file system using an array as back storage.
 */
#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/statfs.h>
#include <linux/buffer_head.h>
#include <linux/backing-dev.h>
#include <linux/kthread.h>
#include <linux/parser.h>
#include <linux/mount.h>
#include <linux/seq_file.h>
#include <linux/proc_fs.h>
#include <linux/random.h>
#include <linux/exportfs.h>
#include <linux/blkdev.h>
#include <linux/quotaops.h>
#include <linux/sysfs.h>
#include <linux/quota.h>

/* Eight directory inodes */
#define ARRAY_FS_NR_DIRINODES (8)

#define ARRAYFS_NR_INODES (32)
#define ARRAYFS_NR_PGS_PER_FILE (8)


struct arrayfs_sb {
	spinlock_t m_lock;
	int mounted;
	struct super_block *sb;
	spinlock_t inode_bmlock;
	unsigned long inode_bm;
};

struct arrayfs_inode {
	struct inode vfs_inode;
};

struct arrayfs_disk_inode {
	umode_t mode;
	unsigned long size;
};

/* These are in-memory inodes. So we don't need to allocate them dynamically. */
struct arrayfs_inode memory_inodes[ARRAYFS_NR_INODES];

/* These are data storage */
struct arrayfs_sb global_sb;
struct arrayfs_disk_inode global_inodes[ARRAYFS_NR_INODES];
char global_data[ARRAYFS_NR_INODES][ARRAYFS_NR_PGS_PER_FILE][4096];

static inline struct arrayfs_inode *ARRAYFS_I(struct inode *inode)
{
	return container_of(inode, struct arrayfs_inode, vfs_inode);
}

static inline struct arrayfs_sb *ARRAYFS_I_SB(struct arrayfs_inode *si)
{
	return &global_sb;
}

const struct inode_operations arrayfs_dir_iops = {

};

const struct inode_operations arrayfs_file_iops = {

};


static struct inode *arrayfs_alloc_inode(struct super_block *sb)
{
	struct arrayfs_sb *sbi = sb->s_fs_info;
	int pa;
	struct arrayfs_inode *si;

	spin_lock(&sbi->inode_bmlock);
	pa = find_first_zero_bit(&sbi->inode_bm, ARRAYFS_NR_INODES);
	if (pa == ARRAYFS_NR_INODES) {
		spin_unlock(&sbi->inode_bmlock);
		return NULL;
	}
	set_bit(pa, &sbi->inode_bm);
	spin_unlock(&sbi->inode_bmlock);

	si = &memory_inodes[pa];

	inode_init_once(&si->vfs_inode);
	pr_notice("%s, allocate new in-memory inode, pa=%d\n",
				__func__, pa);

	return &si->vfs_inode;
}

static void arrayfs_destroy_inode(struct inode *inode)
{
	struct arrayfs_inode *si = ARRAYFS_I(inode);
	int pa = si - memory_inodes;
	//struct arrayfs_sb *sbi = ARRAYFS_I_SB(si);

	pr_notice("%s, %d\n", __func__, pa);

	//spin_lock(&sbi->inode_bmlock);
	//clear_bit(pa, &sbi->inode_bm);
	//spin_unlock(&sbi->inode_bmlock);
}

static void arrayfs_put_super(struct super_block *sb)
{
	spin_lock(&global_sb.m_lock);
	global_sb.mounted = 0;
	spin_unlock(&global_sb.m_lock);
}

static const struct super_operations arrayfs_sops = {
	.alloc_inode	= arrayfs_alloc_inode,
	//.drop_inode	= f2fs_drop_inode,
	.destroy_inode	= arrayfs_destroy_inode,
	//.write_inode	= f2fs_write_inode,
	//.dirty_inode	= f2fs_dirty_inode,
	//.show_options	= f2fs_show_options,
	//.evict_inode	= f2fs_evict_inode,
	.put_super	= arrayfs_put_super,
};

static int arrayfs_read_inode(struct inode *inode)
{
	unsigned long ino = inode->i_ino;
	struct arrayfs_disk_inode *di;

	if (ino >= ARRAYFS_NR_INODES)
		return -EINVAL;

	di = &global_inodes[ino];
	inode->i_mode = di->mode;
	inode->i_size = di->size;
	return 0;
}

static struct inode *arrayfs_iget(struct super_block *sb, unsigned long ino)
{
	struct inode *inode;
	int ret;

	inode = iget_locked(sb, ino);
	if (!inode)
		return ERR_PTR(-ENOMEM);

	if (!(inode->i_state & I_NEW)) {
		return inode;
	}

	ret = arrayfs_read_inode(inode);
	if (ret)
		goto bad_inode;

	if (S_ISREG(inode->i_mode)) {
		inode->i_op = &arrayfs_file_iops;
	} else if (S_ISDIR(inode->i_mode)) {
		inode->i_op = &arrayfs_dir_iops;
	}
	unlock_new_inode(inode);
	return inode;

bad_inode:
	iget_failed(inode);
	return ERR_PTR(ret);
}

static int arrayfs_fill_super(struct super_block *sb, void *data, int silent)
{
	struct arrayfs_sb *sbi;
	struct inode *root_inode;
	int err;

	spin_lock(&global_sb.m_lock);
	if (global_sb.mounted) {
		spin_unlock(&global_sb.m_lock);
		pr_err("%s, already mounted\n",
				__func__);
		return -EBUSY;
	}
	global_sb.mounted = 1;
	spin_unlock(&global_sb.m_lock);
	sb->s_fs_info = sbi = &global_sb;
	sbi->sb = sb;
	sbi->inode_bm = 0;
	spin_lock_init(&sbi->inode_bmlock);
	sb->s_op = &arrayfs_sops;

	/* Deal with root inode */
	root_inode = arrayfs_iget(sb, 0);
	if (IS_ERR(root_inode)) {
		pr_notice("%s, Can't get root inode\n",
					__func__);
		err = PTR_ERR(root_inode);
		goto errout;
	}
	sb->s_root = d_make_root(root_inode);
	if (!sb->s_root) {
		err = -ENOMEM;
		goto errout; //No need to free anything
	}

	pr_notice("%s, Mount arrayfs succceed!\n",
			__func__);

	return 0;

errout:
	spin_lock(&global_sb.m_lock);
	global_sb.mounted = 0;
	spin_unlock(&global_sb.m_lock);
	return err;
}

static struct dentry *arrayfs_mount(struct file_system_type *fs_type, int flags,
			const char *dev_name, void *data)
{
	return mount_nodev(fs_type, flags, data, arrayfs_fill_super);
}

static void arrayfs_umount(struct super_block *sb)
{
	kill_anon_super(sb);
}

static struct file_system_type arrayfs_type = {
	.owner		= THIS_MODULE,
	.name		= "arrayfs",
	.mount		= arrayfs_mount,
	.kill_sb	= arrayfs_umount,
	.fs_flags	= FS_REQUIRES_DEV,
};
MODULE_ALIAS_FS("arrayfs");

static void mkfs_arrayfs(void)
{
	struct arrayfs_disk_inode *di = &global_inodes[0];

	di->mode = S_IFDIR;
	di->size = 0;
}

static int __init init_arrayfs(void)
{
	int err;

	mkfs_arrayfs();

	global_sb.mounted = 0;
	spin_lock_init(&global_sb.m_lock);

	err = register_filesystem(&arrayfs_type);
	if (err)
		goto out;
	pr_notice("%s finished\n", __func__);
	return 0;
out:
	return err;
}

static void __exit exit_arrayfs(void)
{
	pr_notice("%s\n", __func__);
	unregister_filesystem(&arrayfs_type);
}

module_init(init_arrayfs)
module_exit(exit_arrayfs)


MODULE_AUTHOR("Hongwei Qin <glqhw@hust.edu.cn>");
MODULE_LICENSE("MIT");
MODULE_DESCRIPTION("Array File System");

